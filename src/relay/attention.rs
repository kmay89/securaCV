//! The hub attention engine: how loudly to tell the household about one
//! event, without identity (spec/household_keys_v0.md §7).
//!
//! Like the rest of the relay core this is decision, not delivery. [`decide`]
//! is a total, pure function from one [`AttentionInput`] to one
//! [`AttentionDecision`]: a tier (`none`, `digest`, `important`, `critical`),
//! one closed phrase, an episode key and a closed reason. [`EpisodeGovernor`]
//! then decides whether a decision notifies or collapses into its episode,
//! on monotonic seconds the caller passes. No socket, no clock, no `String`
//! in or out.
//!
//! **Nothing calls this yet.** `src/bin/alert_relay.rs` is unchanged:
//! [`super::evaluate`], [`super::SUBSCRIBE_TOPICS`] and the cloud lanes are
//! exactly as they were. This module is the policy, host-tested and pinned to
//! the dictionary, so the wiring that follows has one thing to call.
//!
//! **Dictionary first.** Every vocabulary here — the household posture, the
//! tiers, the zone kinds, the channels and their modality classes, the event
//! classes and their floor and ceiling tiers, the episode families, the
//! phrases, the reasons, the class map from both event dialects and the
//! governor's numbers — is the `household_posture` and `attention` blocks of
//! `spec/witness_dictionary.json`. `scripts/lint_dictionary_sync.py` parses
//! this file and fails CI on any drift, and `spec/fixtures/attention_v1.json`
//! is the conformance vector set every surface that decides tiers replays
//! (the test at the bottom of this file is the Rust replay).
//!
//! The rules (spec §7.2, renumbered here in the order [`decide`] applies
//! them; "rule N" in this file means this list):
//!
//! 1. **Critical is never lowered.** Tamper, a log-integrity failure, a
//!    witness declared dark and a heard smoke or CO cadence are `critical`
//!    whatever the posture, zone, channel, corroboration, novelty, dwell,
//!    hour, quiet hours or episode.
//! 2. **Household posture acts on sensing, not on wording.** While a valid
//!    `home` statement is current, every other event from an `indoor` witness
//!    is `none` — the outcome of that witness sensing tamper only. Outdoor,
//!    door and unclassified zones are never affected. The phrase does not
//!    change.
//! 3. **Some classes never interrupt** (an animal; an event with no word).
//! 4. **Novelty is for radio-only arrivals and nothing else.** An arrival seen
//!    on carried radio alone stays `digest` unless the device's own hint is
//!    `unfamiliar` and the dwell is sustained; the reason and phrase are the
//!    same whatever the hint was.
//! 5. **Corroboration earns interruption.** Two or more independent modality
//!    classes agreeing, or a camera or radar person, may be `important`.
//! 6. A single weak channel, or a single strong one with nothing agreeing, is
//!    `digest`.
//!
//! Quiet hours then hold a `digest` for the morning summary and never hold
//! `important` or `critical` ([`AttentionDecision::held_for_morning`]).
//! Household posture `unset`, `away` and `night` give the same decision in
//! v0: `unset` (no valid statement) is today's behavior, and the engine
//! decides as if household posture did not exist.
//!
//! What is structural rather than promised: every input and output type is
//! `Copy` (so none can hold a `String`, a `Vec` or a payload), no field holds
//! a timestamp, a device id or a credential (the destructuring tests below
//! fail to compile if one is added), and [`cloud_class`] maps only the four
//! existing poke classes — household posture and activity never create a new
//! remote poke class.

use std::collections::HashMap;

use super::PokeClass;
use crate::EventType;

// --------------------------------------------------------------------------
// Household posture
// --------------------------------------------------------------------------

/// The household posture the hub is acting on (dictionary
/// `household_posture`).
///
/// Never bare "posture": Vision events already use that word for body
/// posture. `Unset` is the consumer's value when no signed statement is
/// available or none passes verification; it is never signed and has no wire
/// byte, and it means today's behavior.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum HouseholdPosture {
    /// No valid statement: today's behavior, exactly as `Away`.
    Unset,
    /// Full sensing everywhere. The default and the fail-safe.
    Away,
    /// Indoor witnesses sense tamper only; nothing else changes.
    Home,
    /// Full sensing everywhere; behaves as `Away` in v0.
    Night,
}

impl HouseholdPosture {
    /// Every value, in dictionary order.
    pub const ALL: [HouseholdPosture; 4] = [
        HouseholdPosture::Unset,
        HouseholdPosture::Away,
        HouseholdPosture::Home,
        HouseholdPosture::Night,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            HouseholdPosture::Unset => "unset",
            HouseholdPosture::Away => "away",
            HouseholdPosture::Home => "home",
            HouseholdPosture::Night => "night",
        }
    }

    /// The value with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// The posture byte of the signed statement. `Unset` has none: it is
    /// never signed.
    pub fn wire_byte(self) -> Option<u8> {
        match self {
            HouseholdPosture::Unset => None,
            HouseholdPosture::Away => Some(0x00),
            HouseholdPosture::Home => Some(0x01),
            HouseholdPosture::Night => Some(0x02),
        }
    }

    /// The value a statement's posture byte names. Never `Unset`, and `None`
    /// for a byte no value has — which a consumer must treat as `Unset`.
    pub fn from_wire_byte(byte: u8) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.wire_byte() == Some(byte))
    }
}

// --------------------------------------------------------------------------
// Tiers and zone kinds
// --------------------------------------------------------------------------

/// How loudly a decision reaches the household. Ordered: `None < Digest <
/// Important < Critical`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Tier {
    /// Not surfaced anywhere. Only for a non-critical event from an indoor
    /// witness while household posture is `Home`.
    None,
    /// Pull-only: never pushed, never sounds, waits for the next open and
    /// the morning summary.
    Digest,
    /// Time-sensitive, delivered once, on the LAN only.
    Important,
    /// Delivered through quiet hours, repeats until acknowledged.
    Critical,
}

impl Tier {
    /// Every tier, lowest first.
    pub const ALL: [Tier; 4] = [Tier::None, Tier::Digest, Tier::Important, Tier::Critical];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            Tier::None => "none",
            Tier::Digest => "digest",
            Tier::Important => "important",
            Tier::Critical => "critical",
        }
    }

    /// The tier with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }
}

/// Where a witness is, as the operator classified it. Only `Indoor` is ever
/// affected by household posture; a zone nobody classified is not indoor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ZoneKind {
    /// Inside the home.
    Indoor,
    /// Outside the home.
    Outdoor,
    /// At an entry.
    Door,
    /// Not classified by the operator.
    Unknown,
}

impl ZoneKind {
    /// Every zone kind, in dictionary order.
    pub const ALL: [ZoneKind; 4] = [
        ZoneKind::Indoor,
        ZoneKind::Outdoor,
        ZoneKind::Door,
        ZoneKind::Unknown,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            ZoneKind::Indoor => "indoor",
            ZoneKind::Outdoor => "outdoor",
            ZoneKind::Door => "door",
            ZoneKind::Unknown => "unknown",
        }
    }

    /// The zone kind with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// Does household posture `Home` act on a witness in this kind of zone?
    pub fn posture_applies(self) -> bool {
        matches!(self, ZoneKind::Indoor)
    }
}

// --------------------------------------------------------------------------
// Channels, modality classes, corroboration
// --------------------------------------------------------------------------

/// A physical modality class, after `firmware/common/fusion/sentinel_fusion.h`
/// `Modality`: channels that an evader defeats together count once.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Modality {
    /// Body heat in motion (PIR).
    Thermal,
    /// Active radio reflection (mmWave radar).
    RadioReflection,
    /// Passive Wi-Fi channel perturbation (CSI).
    ChannelPerturb,
    /// Radios a person carries (Wi-Fi and BLE device counts) — one class.
    CarriedRadio,
    /// Light: an ambient-light delta and a camera — one class.
    Optical,
    /// A contact, an enclosure switch, a button.
    Mechanical,
    /// A microphone's cadence classification. Not a sentinel class: the
    /// sentinel has no microphone.
    Acoustic,
}

impl Modality {
    /// Every class, in dictionary order.
    pub const ALL: [Modality; 7] = [
        Modality::Thermal,
        Modality::RadioReflection,
        Modality::ChannelPerturb,
        Modality::CarriedRadio,
        Modality::Optical,
        Modality::Mechanical,
        Modality::Acoustic,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            Modality::Thermal => "thermal",
            Modality::RadioReflection => "radio_reflection",
            Modality::ChannelPerturb => "channel_perturb",
            Modality::CarriedRadio => "carried_radio",
            Modality::Optical => "optical",
            Modality::Mechanical => "mechanical",
            Modality::Acoustic => "acoustic",
        }
    }

    /// The class with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    fn bit(self) -> u8 {
        1 << (self as u8)
    }
}

/// The channel an event came from, or one that agreed with it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Channel {
    /// Passive infrared.
    Pir,
    /// mmWave radar.
    Radar,
    /// Wi-Fi channel-state information.
    WifiCsi,
    /// Aggregate Wi-Fi device count.
    WifiRf,
    /// Aggregate BLE device count.
    Ble,
    /// Ambient-light delta.
    Light,
    /// Camera person detection.
    Vision,
    /// Door or window reed contact.
    Contact,
    /// Accelerometer or enclosure tamper switch.
    Tamper,
    /// A button (a doorbell press). Not a sentinel channel.
    Button,
    /// A microphone. Not a sentinel channel.
    Microphone,
    /// The source carried no channel (a kernel event has no device type).
    /// Never counted as corroboration, and weak: a guess never interrupts.
    Unknown,
}

impl Channel {
    /// Every channel, in dictionary order.
    pub const ALL: [Channel; 12] = [
        Channel::Pir,
        Channel::Radar,
        Channel::WifiCsi,
        Channel::WifiRf,
        Channel::Ble,
        Channel::Light,
        Channel::Vision,
        Channel::Contact,
        Channel::Tamper,
        Channel::Button,
        Channel::Microphone,
        Channel::Unknown,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            Channel::Pir => "pir",
            Channel::Radar => "radar",
            Channel::WifiCsi => "wifi_csi",
            Channel::WifiRf => "wifi_rf",
            Channel::Ble => "ble",
            Channel::Light => "light",
            Channel::Vision => "vision",
            Channel::Contact => "contact",
            Channel::Tamper => "tamper",
            Channel::Button => "button",
            Channel::Microphone => "microphone",
            Channel::Unknown => "unknown",
        }
    }

    /// The channel with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// The modality class this channel counts as. Every sentinel channel's
    /// class is the sentinel's own (`modality_of`), and the linter holds the
    /// two together.
    pub fn modality(self) -> Option<Modality> {
        match self {
            Channel::Pir => Some(Modality::Thermal),
            Channel::Radar => Some(Modality::RadioReflection),
            Channel::WifiCsi => Some(Modality::ChannelPerturb),
            Channel::WifiRf => Some(Modality::CarriedRadio),
            Channel::Ble => Some(Modality::CarriedRadio),
            Channel::Light => Some(Modality::Optical),
            Channel::Vision => Some(Modality::Optical),
            Channel::Contact => Some(Modality::Mechanical),
            Channel::Tamper => Some(Modality::Mechanical),
            Channel::Button => Some(Modality::Mechanical),
            Channel::Microphone => Some(Modality::Acoustic),
            Channel::Unknown => None,
        }
    }

    /// Alone, is this channel at most `digest`? (Radio, sound, motion heat, a
    /// light delta — and an unknown channel.)
    pub fn is_weak(self) -> bool {
        matches!(
            self,
            Channel::Pir
                | Channel::WifiCsi
                | Channel::WifiRf
                | Channel::Ble
                | Channel::Light
                | Channel::Microphone
                | Channel::Unknown
        )
    }

    /// May this channel's person classification be `important` on its own?
    /// (A camera; a radar.)
    pub fn sees_person(self) -> bool {
        matches!(self, Channel::Vision | Channel::Radar)
    }
}

/// How many **distinct modality classes** agreed with an event inside the
/// corroboration window, the reporting channel's own class included.
///
/// A count of classes, never of devices or channels: Wi-Fi and BLE from one
/// phone are one class, so they cannot pass as independent corroboration.
/// `0` means unknown and is treated as a single channel.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CorroborationClasses(u8);

impl CorroborationClasses {
    /// No corroboration data at all.
    pub const UNKNOWN: CorroborationClasses = CorroborationClasses(0);
    /// The cap: the number of modality classes.
    pub const MAX: u8 = 7;

    /// Count the distinct classes among these channels. `Channel::Unknown`
    /// counts for nothing.
    pub fn from_channels<I: IntoIterator<Item = Channel>>(channels: I) -> Self {
        Self::from_modalities(channels.into_iter().filter_map(Channel::modality))
    }

    /// Count the distinct classes among these modalities.
    pub fn from_modalities<I: IntoIterator<Item = Modality>>(modalities: I) -> Self {
        let bits = modalities.into_iter().fold(0u8, |acc, m| acc | m.bit());
        // At most seven bits can be set, so this is already within MAX.
        CorroborationClasses(bits.count_ones() as u8)
    }

    /// A count from a caller that already deduplicated by class, capped.
    pub fn from_count(count: u8) -> Self {
        CorroborationClasses(count.min(Self::MAX))
    }

    /// The number of distinct classes (0 = unknown).
    pub fn count(self) -> u8 {
        self.0
    }
}

/// How long the presence has lasted, banded by the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DwellBand {
    /// No dwell measured.
    None,
    /// Under [`DWELL_SUSTAINED_SECS`].
    Brief,
    /// At least [`DWELL_SUSTAINED_SECS`].
    Sustained,
}

/// Where a dwell becomes sustained — canary-wap `notify.h`
/// `MIN_SUSTAINED_MS`, in seconds (the linter pins both).
pub const DWELL_SUSTAINED_SECS: u64 = 90;

impl DwellBand {
    /// Every band, in dictionary order.
    pub const ALL: [DwellBand; 3] = [DwellBand::None, DwellBand::Brief, DwellBand::Sustained];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            DwellBand::None => "none",
            DwellBand::Brief => "brief",
            DwellBand::Sustained => "sustained",
        }
    }

    /// The band with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// Band a dwell length in whole seconds.
    pub fn from_secs(secs: u64) -> Self {
        match secs {
            0 => DwellBand::None,
            s if s < DWELL_SUSTAINED_SECS => DwellBand::Brief,
            _ => DwellBand::Sustained,
        }
    }
}

/// A device's own coarse novelty hint for an arrival seen on carried radio
/// alone. Internal input only: it defaults to `Unknown`, never reaches a
/// phrase or a reason, and cannot make anything critical. Its producer waits
/// on the ADR the July 2026 audit requires (item E.16).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Novelty {
    /// No hint (the default, and today's only value in practice).
    #[default]
    Unknown,
    /// The device has seen this radio pattern recently.
    Familiar,
    /// The device has not.
    Unfamiliar,
}

impl Novelty {
    /// Every hint, in dictionary order.
    pub const ALL: [Novelty; 3] = [Novelty::Unknown, Novelty::Familiar, Novelty::Unfamiliar];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            Novelty::Unknown => "unknown",
            Novelty::Familiar => "familiar",
            Novelty::Unfamiliar => "unfamiliar",
        }
    }

    /// The hint with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }
}

// --------------------------------------------------------------------------
// Event classes, families, phrases, reasons
// --------------------------------------------------------------------------

/// The coarse class of one event (dictionary `attention.classes`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EventClass {
    /// A witness reports interference with itself (or a panic press).
    Tamper,
    /// The sealed log failed its integrity check.
    Integrity,
    /// A witness was declared dark.
    Dark,
    /// A smoke alarm's T3 cadence was heard.
    SmokeAlarmHeard,
    /// A CO alarm's T4 cadence was heard.
    CoAlarmHeard,
    /// A glass-break cadence was heard.
    GlassBreakHeard,
    /// A camera or radar person.
    Person,
    /// Unclassified presence or motion.
    Presence,
    /// Someone stayed a while.
    Dwelling,
    /// A vehicle.
    Vehicle,
    /// An animal or a small object.
    Animal,
    /// A door or window contact changed.
    Contact,
    /// A doorbell.
    Doorbell,
    /// A knock was heard.
    Knock,
    /// An object left the zone.
    ObjectRemoved,
    /// A loud impulse was heard.
    SoundHeard,
    /// A word this hub has no class for.
    Other,
}

impl EventClass {
    /// Every class, in dictionary order.
    pub const ALL: [EventClass; 17] = [
        EventClass::Tamper,
        EventClass::Integrity,
        EventClass::Dark,
        EventClass::SmokeAlarmHeard,
        EventClass::CoAlarmHeard,
        EventClass::GlassBreakHeard,
        EventClass::Person,
        EventClass::Presence,
        EventClass::Dwelling,
        EventClass::Vehicle,
        EventClass::Animal,
        EventClass::Contact,
        EventClass::Doorbell,
        EventClass::Knock,
        EventClass::ObjectRemoved,
        EventClass::SoundHeard,
        EventClass::Other,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            EventClass::Tamper => "tamper",
            EventClass::Integrity => "integrity",
            EventClass::Dark => "dark",
            EventClass::SmokeAlarmHeard => "smoke_alarm_heard",
            EventClass::CoAlarmHeard => "co_alarm_heard",
            EventClass::GlassBreakHeard => "glass_break_heard",
            EventClass::Person => "person",
            EventClass::Presence => "presence",
            EventClass::Dwelling => "dwelling",
            EventClass::Vehicle => "vehicle",
            EventClass::Animal => "animal",
            EventClass::Contact => "contact",
            EventClass::Doorbell => "doorbell",
            EventClass::Knock => "knock",
            EventClass::ObjectRemoved => "object_removed",
            EventClass::SoundHeard => "sound_heard",
            EventClass::Other => "other",
        }
    }

    /// The class with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// The tier this class gets when nothing earns it more. A critical floor
    /// is never lowered.
    pub fn floor_tier(self) -> Tier {
        match self {
            EventClass::Tamper
            | EventClass::Integrity
            | EventClass::Dark
            | EventClass::SmokeAlarmHeard
            | EventClass::CoAlarmHeard => Tier::Critical,
            EventClass::GlassBreakHeard
            | EventClass::Person
            | EventClass::Presence
            | EventClass::Dwelling
            | EventClass::Vehicle
            | EventClass::Animal
            | EventClass::Contact
            | EventClass::Doorbell
            | EventClass::Knock
            | EventClass::ObjectRemoved
            | EventClass::SoundHeard
            | EventClass::Other => Tier::Digest,
        }
    }

    /// The most this class can earn.
    pub fn ceiling_tier(self) -> Tier {
        match self {
            EventClass::Tamper
            | EventClass::Integrity
            | EventClass::Dark
            | EventClass::SmokeAlarmHeard
            | EventClass::CoAlarmHeard => Tier::Critical,
            EventClass::GlassBreakHeard
            | EventClass::Person
            | EventClass::Presence
            | EventClass::Dwelling
            | EventClass::Vehicle
            | EventClass::Contact
            | EventClass::Doorbell
            | EventClass::Knock
            | EventClass::ObjectRemoved
            | EventClass::SoundHeard => Tier::Important,
            EventClass::Animal | EventClass::Other => Tier::Digest,
        }
    }

    /// Is this class always critical?
    pub fn is_critical(self) -> bool {
        self.floor_tier() == Tier::Critical
    }

    /// May a camera or radar person classification make this class
    /// `important` on its own?
    pub fn person_class(self) -> bool {
        matches!(self, EventClass::Person | EventClass::Dwelling)
    }

    /// The episode family this class belongs to.
    pub fn family(self) -> EpisodeFamily {
        match self {
            EventClass::Tamper => EpisodeFamily::Tamper,
            EventClass::Integrity => EpisodeFamily::Integrity,
            EventClass::Dark => EpisodeFamily::Dark,
            EventClass::SmokeAlarmHeard | EventClass::CoAlarmHeard => EpisodeFamily::LifeSafety,
            EventClass::GlassBreakHeard => EpisodeFamily::Breakage,
            EventClass::Person | EventClass::Presence | EventClass::Dwelling => {
                EpisodeFamily::Someone
            }
            EventClass::Vehicle => EpisodeFamily::Vehicle,
            EventClass::Animal => EpisodeFamily::Animal,
            EventClass::Contact => EpisodeFamily::Opening,
            EventClass::Doorbell | EventClass::Knock => EpisodeFamily::Visitor,
            EventClass::ObjectRemoved => EpisodeFamily::Object,
            EventClass::SoundHeard => EpisodeFamily::Sound,
            EventClass::Other => EpisodeFamily::Other,
        }
    }

    /// The class's one phrase. Only the class chooses the wording, so
    /// neither household posture nor novelty can change it.
    pub fn phrase(self) -> PhraseId {
        match self {
            EventClass::Tamper => PhraseId::TamperAtZone,
            EventClass::Integrity => PhraseId::IntegrityFailed,
            EventClass::Dark => PhraseId::DarkAtZone,
            EventClass::SmokeAlarmHeard => PhraseId::SmokeHeardAtZone,
            EventClass::CoAlarmHeard => PhraseId::CoHeardAtZone,
            EventClass::GlassBreakHeard => PhraseId::GlassBreakHeardAtZone,
            EventClass::Person => PhraseId::PersonAtZone,
            EventClass::Presence => PhraseId::ActivityAtZone,
            EventClass::Dwelling => PhraseId::StayedAtZone,
            EventClass::Vehicle => PhraseId::VehicleAtZone,
            EventClass::Animal => PhraseId::AnimalAtZone,
            EventClass::Contact => PhraseId::OpeningAtZone,
            EventClass::Doorbell => PhraseId::DoorbellAtZone,
            EventClass::Knock => PhraseId::KnockHeardAtZone,
            EventClass::ObjectRemoved => PhraseId::ObjectRemovedAtZone,
            EventClass::SoundHeard => PhraseId::SoundHeardAtZone,
            EventClass::Other => PhraseId::EventAtZone,
        }
    }
}

/// What an episode is keyed on, with the zone (dictionary
/// `attention.episode_families`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EpisodeFamily {
    /// Tamper.
    Tamper,
    /// Log integrity.
    Integrity,
    /// A dark witness.
    Dark,
    /// A heard smoke or CO alarm.
    LifeSafety,
    /// Person, presence, dwelling: one visit.
    Someone,
    /// A doorbell or a knock.
    Visitor,
    /// A glass break.
    Breakage,
    /// A loud impulse.
    Sound,
    /// A door or window.
    Opening,
    /// A vehicle.
    Vehicle,
    /// An animal.
    Animal,
    /// An object removed.
    Object,
    /// Anything unclassified.
    Other,
}

impl EpisodeFamily {
    /// Every family, in dictionary order.
    pub const ALL: [EpisodeFamily; 13] = [
        EpisodeFamily::Tamper,
        EpisodeFamily::Integrity,
        EpisodeFamily::Dark,
        EpisodeFamily::LifeSafety,
        EpisodeFamily::Someone,
        EpisodeFamily::Visitor,
        EpisodeFamily::Breakage,
        EpisodeFamily::Sound,
        EpisodeFamily::Opening,
        EpisodeFamily::Vehicle,
        EpisodeFamily::Animal,
        EpisodeFamily::Object,
        EpisodeFamily::Other,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            EpisodeFamily::Tamper => "tamper",
            EpisodeFamily::Integrity => "integrity",
            EpisodeFamily::Dark => "dark",
            EpisodeFamily::LifeSafety => "life_safety",
            EpisodeFamily::Someone => "someone",
            EpisodeFamily::Visitor => "visitor",
            EpisodeFamily::Breakage => "breakage",
            EpisodeFamily::Sound => "sound",
            EpisodeFamily::Opening => "opening",
            EpisodeFamily::Vehicle => "vehicle",
            EpisodeFamily::Animal => "animal",
            EpisodeFamily::Object => "object",
            EpisodeFamily::Other => "other",
        }
    }

    /// The family with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }
}

/// One closed phrase (dictionary `attention.phrase_ids`). No `String`
/// constructor exists; the only blank is `{zone}`, the operator's own name
/// for the zone, which the caller fills from the operator's zone table —
/// never from a topic or a payload.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PhraseId {
    /// "Tamper reported at {zone}".
    TamperAtZone,
    /// "The witness log failed its integrity check".
    IntegrityFailed,
    /// "A witness at {zone} went dark".
    DarkAtZone,
    /// "Smoke alarm heard at {zone}".
    SmokeHeardAtZone,
    /// "CO alarm heard at {zone}".
    CoHeardAtZone,
    /// "Glass break heard at {zone}".
    GlassBreakHeardAtZone,
    /// "Someone at {zone}".
    PersonAtZone,
    /// "Activity at {zone}".
    ActivityAtZone,
    /// "Someone stayed a while at {zone}".
    StayedAtZone,
    /// "Vehicle at {zone}".
    VehicleAtZone,
    /// "Animal at {zone}".
    AnimalAtZone,
    /// "Door or window activity at {zone}".
    OpeningAtZone,
    /// "Doorbell at {zone}".
    DoorbellAtZone,
    /// "Knock heard at {zone}".
    KnockHeardAtZone,
    /// "Object removed from {zone}".
    ObjectRemovedAtZone,
    /// "Loud sound heard at {zone}".
    SoundHeardAtZone,
    /// "Event at {zone}".
    EventAtZone,
}

impl PhraseId {
    /// Every phrase, in dictionary order.
    pub const ALL: [PhraseId; 17] = [
        PhraseId::TamperAtZone,
        PhraseId::IntegrityFailed,
        PhraseId::DarkAtZone,
        PhraseId::SmokeHeardAtZone,
        PhraseId::CoHeardAtZone,
        PhraseId::GlassBreakHeardAtZone,
        PhraseId::PersonAtZone,
        PhraseId::ActivityAtZone,
        PhraseId::StayedAtZone,
        PhraseId::VehicleAtZone,
        PhraseId::AnimalAtZone,
        PhraseId::OpeningAtZone,
        PhraseId::DoorbellAtZone,
        PhraseId::KnockHeardAtZone,
        PhraseId::ObjectRemovedAtZone,
        PhraseId::SoundHeardAtZone,
        PhraseId::EventAtZone,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            PhraseId::TamperAtZone => "tamper_at_zone",
            PhraseId::IntegrityFailed => "integrity_failed",
            PhraseId::DarkAtZone => "dark_at_zone",
            PhraseId::SmokeHeardAtZone => "smoke_heard_at_zone",
            PhraseId::CoHeardAtZone => "co_heard_at_zone",
            PhraseId::GlassBreakHeardAtZone => "glass_break_heard_at_zone",
            PhraseId::PersonAtZone => "person_at_zone",
            PhraseId::ActivityAtZone => "activity_at_zone",
            PhraseId::StayedAtZone => "stayed_at_zone",
            PhraseId::VehicleAtZone => "vehicle_at_zone",
            PhraseId::AnimalAtZone => "animal_at_zone",
            PhraseId::OpeningAtZone => "opening_at_zone",
            PhraseId::DoorbellAtZone => "doorbell_at_zone",
            PhraseId::KnockHeardAtZone => "knock_heard_at_zone",
            PhraseId::ObjectRemovedAtZone => "object_removed_at_zone",
            PhraseId::SoundHeardAtZone => "sound_heard_at_zone",
            PhraseId::EventAtZone => "event_at_zone",
        }
    }

    /// The phrase with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// The phrase's text, with `{zone}` as its only blank.
    pub fn text(self) -> &'static str {
        match self {
            PhraseId::TamperAtZone => "Tamper reported at {zone}",
            PhraseId::IntegrityFailed => "The witness log failed its integrity check",
            PhraseId::DarkAtZone => "A witness at {zone} went dark",
            PhraseId::SmokeHeardAtZone => "Smoke alarm heard at {zone}",
            PhraseId::CoHeardAtZone => "CO alarm heard at {zone}",
            PhraseId::GlassBreakHeardAtZone => "Glass break heard at {zone}",
            PhraseId::PersonAtZone => "Someone at {zone}",
            PhraseId::ActivityAtZone => "Activity at {zone}",
            PhraseId::StayedAtZone => "Someone stayed a while at {zone}",
            PhraseId::VehicleAtZone => "Vehicle at {zone}",
            PhraseId::AnimalAtZone => "Animal at {zone}",
            PhraseId::OpeningAtZone => "Door or window activity at {zone}",
            PhraseId::DoorbellAtZone => "Doorbell at {zone}",
            PhraseId::KnockHeardAtZone => "Knock heard at {zone}",
            PhraseId::ObjectRemovedAtZone => "Object removed from {zone}",
            PhraseId::SoundHeardAtZone => "Loud sound heard at {zone}",
            PhraseId::EventAtZone => "Event at {zone}",
        }
    }
}

/// Why a decision came out as it did — the "why did or didn't I get this"
/// trace. It names the rule, never an input the rule must not reveal.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ReasonCode {
    /// Rule 1: a critical class.
    CriticalClass,
    /// Rule 2: household posture home, indoor witness, applied by the hub.
    HomeIndoor,
    /// Rule 2, where the witness itself was sensing tamper only.
    HomeIndoorAtWitness,
    /// Rule 3: a class whose ceiling is digest.
    ClassCeiling,
    /// Rule 3: a word this hub has no class for.
    Unclassified,
    /// Rule 4: an arrival on carried radio alone (whatever the novelty
    /// hint was).
    RfOnlyArrival,
    /// Rule 5: two or more independent modality classes agreed.
    Corroborated,
    /// Rule 5: a camera or radar person.
    CameraOrRadarPerson,
    /// Rule 6: one weak channel alone.
    SingleWeakChannel,
    /// Rule 6: one strong channel with nothing agreeing.
    Uncorroborated,
}

impl ReasonCode {
    /// Every reason, in dictionary order.
    pub const ALL: [ReasonCode; 10] = [
        ReasonCode::CriticalClass,
        ReasonCode::HomeIndoor,
        ReasonCode::HomeIndoorAtWitness,
        ReasonCode::ClassCeiling,
        ReasonCode::Unclassified,
        ReasonCode::RfOnlyArrival,
        ReasonCode::Corroborated,
        ReasonCode::CameraOrRadarPerson,
        ReasonCode::SingleWeakChannel,
        ReasonCode::Uncorroborated,
    ];

    /// The dictionary id.
    pub fn id(self) -> &'static str {
        match self {
            ReasonCode::CriticalClass => "critical_class",
            ReasonCode::HomeIndoor => "home_indoor",
            ReasonCode::HomeIndoorAtWitness => "home_indoor_at_witness",
            ReasonCode::ClassCeiling => "class_ceiling",
            ReasonCode::Unclassified => "unclassified",
            ReasonCode::RfOnlyArrival => "rf_only_arrival",
            ReasonCode::Corroborated => "corroborated",
            ReasonCode::CameraOrRadarPerson => "camera_or_radar_person",
            ReasonCode::SingleWeakChannel => "single_weak_channel",
            ReasonCode::Uncorroborated => "uncorroborated",
        }
    }

    /// The reason with this dictionary id.
    pub fn from_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.id() == id)
    }

    /// The trace sentence the owner reads.
    pub fn text(self) -> &'static str {
        match self {
            ReasonCode::CriticalClass => {
                "Tamper, a log failure, a dark witness and a heard smoke or CO alarm are always critical."
            }
            ReasonCode::HomeIndoor => "Home posture: indoor witnesses report tamper only.",
            ReasonCode::HomeIndoorAtWitness => {
                "Home posture: this witness was sensing tamper only."
            }
            ReasonCode::ClassCeiling => "This kind of event never interrupts on its own.",
            ReasonCode::Unclassified => {
                "An event this hub has no word for is never more than a digest."
            }
            ReasonCode::RfOnlyArrival => "Seen on carried radio alone.",
            ReasonCode::Corroborated => "Two or more independent kinds of sensing agreed.",
            ReasonCode::CameraOrRadarPerson => "A camera or a radar saw a person.",
            ReasonCode::SingleWeakChannel => {
                "One weak channel alone (radio, sound, motion heat or light) is never more than a digest."
            }
            ReasonCode::Uncorroborated => "Nothing independent agreed, so it waits in the digest.",
        }
    }
}

// --------------------------------------------------------------------------
// The class map: both event dialects
// --------------------------------------------------------------------------

/// The class of a kernel event. Exhaustive on purpose: a new `EventType`
/// variant does not compile until it is mapped here (and in the dictionary's
/// `class_map.kernel_event_types`).
pub fn class_for_event_type(event: &EventType) -> EventClass {
    match event {
        EventType::BoundaryCrossingObjectLarge => EventClass::Presence,
        EventType::BoundaryCrossingObjectSmall => EventClass::Animal,
        EventType::AcousticImpulseInZone => EventClass::SoundHeard,
        EventType::PresenceInRestrictedZone => EventClass::Presence,
        EventType::VehiclePresenceAfterHours => EventClass::Vehicle,
        EventType::ContactStateChange => EventClass::Contact,
        EventType::ObjectRemovedFromZone => EventClass::ObjectRemoved,
        EventType::TamperDetected => EventClass::Tamper,
        EventType::VehicleArrivalDeparture => EventClass::Vehicle,
    }
}

/// The class of a Canary device wire word. Exact and case-sensitive; any word
/// not listed is `Other`, whose ceiling is `digest` — an unknown word can
/// never be critical, and never `none`.
pub fn class_for_device_word(word: &str) -> EventClass {
    match word {
        "person_detected" => EventClass::Person,
        "presence_started" => EventClass::Person,
        "dwell_started" => EventClass::Dwelling,
        "interaction_likely" => EventClass::Dwelling,
        "rf_presence_started" => EventClass::Presence,
        "rf_dwell_started" => EventClass::Dwelling,
        "motion_detected" => EventClass::Presence,
        "vehicle_detected" => EventClass::Vehicle,
        "animal_detected" => EventClass::Animal,
        "doorbell" => EventClass::Doorbell,
        "knock" => EventClass::Knock,
        "glass_break" => EventClass::GlassBreakHeard,
        "smoke_alarm_t3" => EventClass::SmokeAlarmHeard,
        "co_alarm_t4" => EventClass::CoAlarmHeard,
        "tamper" => EventClass::Tamper,
        "panic" => EventClass::Tamper,
        "witness_lost" => EventClass::Dark,
        "chain_break" => EventClass::Integrity,
        "verify_failed" => EventClass::Integrity,
        _ => EventClass::Other,
    }
}

// --------------------------------------------------------------------------
// Input, decision, engine
// --------------------------------------------------------------------------

/// The operator's zone, as a slot in the operator's own zone table. A small
/// index, not a name and not a device id; the phrase's `{zone}` blank is
/// filled from the table entry it points at.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ZoneSlot(pub u8);

impl ZoneSlot {
    /// No zone: a hub-level event, or a witness the operator has not placed.
    pub const UNASSIGNED: ZoneSlot = ZoneSlot(0);
}

/// What an episode is keyed on: a family and a zone slot. No device id, no
/// time, no free text.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct EpisodeKey {
    /// The family of the event's class.
    pub family: EpisodeFamily,
    /// The event's zone slot.
    pub zone: ZoneSlot,
}

/// Everything [`decide`] reads about one event. There is no credential, no
/// device id, no timestamp and no payload text here, and the type is `Copy`,
/// so none can be added without breaking the tests below.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct AttentionInput {
    /// The event's coarse class ([`class_for_event_type`],
    /// [`class_for_device_word`]).
    pub class: EventClass,
    /// The event's zone slot.
    pub zone: ZoneSlot,
    /// The operator's classification of that zone.
    pub zone_kind: ZoneKind,
    /// The household posture: the same value for every event at that moment.
    pub posture: HouseholdPosture,
    /// Does the reporting witness apply household posture itself? (Then it
    /// sends no non-critical event while home and indoor, and the rule that
    /// would have made it `none` is moot; it is still `none`.)
    pub witness_honors_posture: bool,
    /// The reporting channel; `Channel::Unknown` when the source carries none.
    pub channel: Channel,
    /// Distinct modality classes that agreed, the reporting one included.
    pub corroboration: CorroborationClasses,
    /// How long the presence has lasted.
    pub dwell: DwellBand,
    /// The device's own novelty hint (read only for radio-only arrivals).
    pub novelty: Novelty,
    /// The hub's local hour at receipt, 0-23. Never emitted. v0 reads it
    /// nowhere — it is held for the after-hours rule set `Night` is reserved
    /// for, and a test proves it changes nothing today.
    pub hour_bucket: u8,
    /// Whether the household's quiet hours are on, by the hub's clock at
    /// receipt.
    pub quiet_hours: bool,
}

/// One decision. Closed values only: no timestamp, no device id, no
/// credential, no free text — and `Copy`, so no field can hold a `String`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct AttentionDecision {
    /// How loudly.
    pub tier: Tier,
    /// What to say (the class's phrase).
    pub phrase: PhraseId,
    /// Which episode it belongs to.
    pub episode: EpisodeKey,
    /// Which rule decided the tier.
    pub reason: ReasonCode,
    /// Quiet hours are holding this digest for the morning summary. Never
    /// true for `important` or `critical`, never true for `none` (nothing to
    /// hold).
    pub held_for_morning: bool,
}

/// Decide how loudly to surface one event. Total and pure.
pub fn decide(input: &AttentionInput) -> AttentionDecision {
    let class = input.class;
    let (tier, reason) = tier_and_reason(input);
    AttentionDecision {
        tier,
        phrase: class.phrase(),
        episode: EpisodeKey {
            family: class.family(),
            zone: input.zone,
        },
        reason,
        held_for_morning: input.quiet_hours && tier == Tier::Digest,
    }
}

fn tier_and_reason(input: &AttentionInput) -> (Tier, ReasonCode) {
    let class = input.class;

    // Rule 1. Nothing below this line can see a critical class.
    if class.is_critical() {
        return (Tier::Critical, ReasonCode::CriticalClass);
    }

    // Rule 2. Household posture acts on sensing: an indoor witness while home
    // senses tamper only, so everything else it reports is not surfaced.
    if input.posture == HouseholdPosture::Home && input.zone_kind.posture_applies() {
        let reason = if input.witness_honors_posture {
            ReasonCode::HomeIndoorAtWitness
        } else {
            ReasonCode::HomeIndoor
        };
        return (Tier::None, reason);
    }

    // Rule 3. Classes that never interrupt.
    let ceiling = class.ceiling_tier();
    if ceiling <= Tier::Digest {
        let reason = if class == EventClass::Other {
            ReasonCode::Unclassified
        } else {
            ReasonCode::ClassCeiling
        };
        return (Tier::Digest, reason);
    }

    let alone = input.corroboration.count() <= 1;

    // Rule 4. Novelty may raise a radio-only arrival, familiarity may hold
    // it at digest — and nothing else ever reads the hint. The reason is the
    // same whichever hint it was.
    if alone
        && input.channel.modality() == Some(Modality::CarriedRadio)
        && class.family() == EpisodeFamily::Someone
    {
        let earned = input.novelty == Novelty::Unfamiliar && input.dwell == DwellBand::Sustained;
        let tier = if earned {
            Tier::Important.min(ceiling)
        } else {
            Tier::Digest
        };
        return (tier, ReasonCode::RfOnlyArrival);
    }

    // Rule 5. Corroboration earns interruption.
    if !alone {
        return (ceiling, ReasonCode::Corroborated);
    }
    if class.person_class() && input.channel.sees_person() {
        return (ceiling, ReasonCode::CameraOrRadarPerson);
    }

    // Rule 6. One channel alone.
    if input.channel.is_weak() {
        (Tier::Digest, ReasonCode::SingleWeakChannel)
    } else {
        (Tier::Digest, ReasonCode::Uncorroborated)
    }
}

/// The relay poke class a decision may travel as, if any.
///
/// `Some` only for a critical decision of the four critical families, and
/// only as one of the four classes the cloud lanes already carry. Household
/// posture and activity never create a remote poke class: an `important`
/// person stays on the LAN (src/relay/mod.rs, `SUBSCRIBE_TOPICS`: widening
/// the cloud vocabulary is a privacy decision, not a tweak).
pub fn cloud_class(decision: &AttentionDecision) -> Option<PokeClass> {
    if decision.tier != Tier::Critical {
        return None;
    }
    match decision.episode.family {
        EpisodeFamily::Tamper => Some(PokeClass::Tamper),
        EpisodeFamily::Integrity => Some(PokeClass::Integrity),
        EpisodeFamily::Dark => Some(PokeClass::Offline),
        EpisodeFamily::LifeSafety => Some(PokeClass::Pattern),
        EpisodeFamily::Someone
        | EpisodeFamily::Visitor
        | EpisodeFamily::Breakage
        | EpisodeFamily::Sound
        | EpisodeFamily::Opening
        | EpisodeFamily::Vehicle
        | EpisodeFamily::Animal
        | EpisodeFamily::Object
        | EpisodeFamily::Other => None,
    }
}

// --------------------------------------------------------------------------
// Episodes
// --------------------------------------------------------------------------

/// First rest after an episode's opening notification, in seconds.
pub const EPISODE_BASE_COOLDOWN_SECS: u64 = 120;
/// Rests double per notification up to this ceiling, in seconds.
pub const EPISODE_COOLDOWN_CAP_SECS: u64 = 1800;
/// This long with no repeat even considered ends the episode, in seconds.
pub const EPISODE_CALM_GAP_SECS: u64 = 1800;

/// Why a decision notifies.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum NotifyCause {
    /// The first of an episode (or the first after a calm gap).
    FirstOfEpisode,
    /// A higher tier than anything already notified in the episode.
    Escalation,
    /// Critical is never governed.
    Critical,
    /// A same-or-lower repeat whose earned rest has passed.
    RestElapsed,
}

/// What the governor says about one decision.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EpisodeVerdict {
    /// Surface it.
    Notify(NotifyCause),
    /// Collapsed into the episode's earlier notification; a same-or-lower
    /// repeat may notify again after this many seconds.
    Collapsed {
        /// Seconds left in the earned rest.
        resting_for_secs: u64,
    },
    /// Tier `none`: not surfaced, and not considered — it neither starts an
    /// episode nor keeps one from calming down.
    NotSurfaced,
}

/// One episode's memory. RAM only; never emitted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct EpisodeMemory {
    last_notified_at: u64,
    /// The last time any repeat was considered, notified or collapsed. The
    /// calm gap is measured from here, so a family that never stops churning
    /// can never age into a fake calm (the hole RepeatGovernor.swift fixed).
    last_seen_at: u64,
    worst_notified: Tier,
    notifications: u32,
}

/// One episode, one notification: the repeat governor, keyed by
/// [`EpisodeKey`] the way the relay's `Debouncer` is keyed by (class,
/// device), on the caller's monotonic seconds.
///
/// The numbers and the shape are the Apple apps' `RepeatGovernor` (120 s
/// doubling to a 1800 s ceiling, an 1800 s calm gap, escalation always
/// notifies, the critical tier is never governed), so hub and phone rest a
/// burst the same way. Memory is bounded by its key: 13 families times 256
/// zone slots.
#[derive(Debug, Default)]
pub struct EpisodeGovernor {
    episodes: HashMap<EpisodeKey, EpisodeMemory>,
}

impl EpisodeGovernor {
    /// The rest an episode has earned after this many notifications.
    pub fn cooldown_after(notifications: u32) -> u64 {
        match notifications {
            0 => 0,
            n if n > 16 => EPISODE_COOLDOWN_CAP_SECS,
            n => (EPISODE_BASE_COOLDOWN_SECS << (n - 1)).min(EPISODE_COOLDOWN_CAP_SECS),
        }
    }

    /// Should this decision notify, given its episode so far? Records what it
    /// saw. Seconds must be monotonic; a clock that steps back can only make
    /// a repeat rest longer, never notify more.
    pub fn consider(&mut self, key: EpisodeKey, tier: Tier, now_secs: u64) -> EpisodeVerdict {
        if tier == Tier::None {
            return EpisodeVerdict::NotSurfaced;
        }
        let prior = self.episodes.get(&key).copied();

        // The critical tier is never governed.
        if tier == Tier::Critical {
            let memory = EpisodeMemory {
                last_notified_at: now_secs,
                last_seen_at: now_secs,
                worst_notified: prior.map_or(tier, |m| m.worst_notified.max(tier)),
                notifications: prior.map_or(0, |m| m.notifications).saturating_add(1),
            };
            self.episodes.insert(key, memory);
            return EpisodeVerdict::Notify(NotifyCause::Critical);
        }

        // No episode, or the last one ended calmly: a fresh story.
        let memory = match prior {
            Some(m) if now_secs.saturating_sub(m.last_seen_at) < EPISODE_CALM_GAP_SECS => m,
            _ => {
                let fresh = EpisodeMemory {
                    last_notified_at: now_secs,
                    last_seen_at: now_secs,
                    worst_notified: tier,
                    notifications: 1,
                };
                self.episodes.insert(key, fresh);
                return EpisodeVerdict::Notify(NotifyCause::FirstOfEpisode);
            }
        };

        // A higher tier always gets through.
        if tier > memory.worst_notified {
            let escalated = EpisodeMemory {
                last_notified_at: now_secs,
                last_seen_at: now_secs,
                worst_notified: tier,
                notifications: memory.notifications.saturating_add(1),
            };
            self.episodes.insert(key, escalated);
            return EpisodeVerdict::Notify(NotifyCause::Escalation);
        }

        // A same-or-lower repeat: only once its earned rest has passed.
        let rest = Self::cooldown_after(memory.notifications);
        let elapsed = now_secs.saturating_sub(memory.last_notified_at);
        if elapsed >= rest {
            let repeated = EpisodeMemory {
                last_notified_at: now_secs,
                last_seen_at: now_secs,
                worst_notified: memory.worst_notified.max(tier),
                notifications: memory.notifications.saturating_add(1),
            };
            self.episodes.insert(key, repeated);
            return EpisodeVerdict::Notify(NotifyCause::RestElapsed);
        }

        // Collapsed, but seen: the calm gap keeps measuring from now.
        let seen = EpisodeMemory {
            last_seen_at: now_secs,
            ..memory
        };
        self.episodes.insert(key, seen);
        EpisodeVerdict::Collapsed {
            resting_for_secs: rest - elapsed,
        }
    }

    /// [`EpisodeGovernor::consider`] for a decision.
    pub fn consider_decision(
        &mut self,
        decision: &AttentionDecision,
        now_secs: u64,
    ) -> EpisodeVerdict {
        self.consider(decision.episode, decision.tier, now_secs)
    }

    /// Drop every episode that has been calm for the calm gap. Such an
    /// episode is already over, and what its memory still holds is when a
    /// zone last saw activity — which the hub has no reason to keep.
    pub fn forget_calm(&mut self, now_secs: u64) {
        self.episodes
            .retain(|_, m| now_secs.saturating_sub(m.last_seen_at) < EPISODE_CALM_GAP_SECS);
    }

    /// How many episodes are remembered (for tests and health counters).
    pub fn len(&self) -> usize {
        self.episodes.len()
    }

    /// Whether no episode is remembered.
    pub fn is_empty(&self) -> bool {
        self.episodes.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;
    use std::path::PathBuf;

    // ---------------------------------------------------------------- helpers

    /// Every input the invariant tests sweep: each class, zone kind,
    /// household posture, honors flag, channel, corroboration count, dwell,
    /// novelty and quiet flag, at one hour (the hour is proved inert
    /// separately).
    fn every_input() -> Vec<AttentionInput> {
        let mut out = Vec::new();
        for class in EventClass::ALL {
            for zone_kind in ZoneKind::ALL {
                for posture in HouseholdPosture::ALL {
                    for witness_honors_posture in [false, true] {
                        for channel in Channel::ALL {
                            for count in 0..=CorroborationClasses::MAX {
                                for dwell in DwellBand::ALL {
                                    for novelty in Novelty::ALL {
                                        for quiet_hours in [false, true] {
                                            out.push(AttentionInput {
                                                class,
                                                zone: ZoneSlot(3),
                                                zone_kind,
                                                posture,
                                                witness_honors_posture,
                                                channel,
                                                corroboration: CorroborationClasses::from_count(
                                                    count,
                                                ),
                                                dwell,
                                                novelty,
                                                hour_bucket: 14,
                                                quiet_hours,
                                            });
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        out
    }

    fn base_input(class: EventClass, channel: Channel) -> AttentionInput {
        AttentionInput {
            class,
            zone: ZoneSlot(1),
            zone_kind: ZoneKind::Outdoor,
            posture: HouseholdPosture::Unset,
            witness_honors_posture: false,
            channel,
            corroboration: CorroborationClasses::from_channels([channel]),
            dwell: DwellBand::None,
            novelty: Novelty::Unknown,
            hour_bucket: 14,
            quiet_hours: false,
        }
    }

    // ------------------------------------------------- structural invariants

    fn assert_copy<T: Copy>() {}

    #[test]
    fn the_input_has_no_credential_field() {
        // Exhaustive destructuring, no `..`: adding any field — a credential,
        // a handle, a device id, a timestamp — fails to compile here, and
        // whoever adds it has to read why. A household key is not an input
        // to attention; household posture is, and it is one value for every
        // event at that moment.
        let input = base_input(EventClass::Person, Channel::Vision);
        let AttentionInput {
            class: _,
            zone: _,
            zone_kind: _,
            posture: _,
            witness_honors_posture: _,
            channel: _,
            corroboration: _,
            dwell: _,
            novelty: _,
            hour_bucket: _,
            quiet_hours: _,
        } = input;
        // Copy: no String, Vec or Box can hide inside it.
        assert_copy::<AttentionInput>();
    }

    #[test]
    fn no_decision_field_can_hold_a_timestamp_device_id_or_payload() {
        let decision = decide(&base_input(EventClass::Person, Channel::Vision));
        // Exhaustive destructuring with the closed types spelled out: a new
        // field, or a field widened to a String or an integer clock, fails
        // to compile.
        let AttentionDecision {
            tier,
            phrase,
            episode,
            reason,
            held_for_morning,
        } = decision;
        let _: (Tier, PhraseId, ReasonCode, bool) = (tier, phrase, reason, held_for_morning);
        let EpisodeKey { family, zone } = episode;
        let _: (EpisodeFamily, ZoneSlot) = (family, zone);
        let ZoneSlot(slot) = zone;
        // A zone slot is a table index, not a device id: it is a byte.
        let _: u8 = slot;
        assert_copy::<AttentionDecision>();
        assert_copy::<EpisodeKey>();
    }

    // ---------------------------------------------------------- rule sweeps

    #[test]
    fn critical_classes_are_always_critical() {
        for input in every_input() {
            let d = decide(&input);
            if input.class.is_critical() {
                assert_eq!(d.tier, Tier::Critical, "{input:?}");
                assert_eq!(d.reason, ReasonCode::CriticalClass, "{input:?}");
                assert!(cloud_class(&d).is_some(), "{input:?}");
            } else {
                // And nothing else ever is.
                assert!(d.tier < Tier::Critical, "{input:?}");
                assert_eq!(cloud_class(&d), None, "{input:?}");
            }
        }
    }

    #[test]
    fn quiet_hours_hold_digest_only_and_never_critical() {
        for input in every_input() {
            let d = decide(&input);
            assert_eq!(
                d.held_for_morning,
                input.quiet_hours && d.tier == Tier::Digest,
                "{input:?}"
            );
            // Quiet hours never change the tier, the phrase or the reason.
            let other = decide(&AttentionInput {
                quiet_hours: !input.quiet_hours,
                ..input
            });
            assert_eq!(
                (d.tier, d.phrase, d.reason, d.episode),
                (other.tier, other.phrase, other.reason, other.episode)
            );
        }
    }

    #[test]
    fn novelty_never_yields_critical_and_never_changes_phrase_or_reason() {
        for input in every_input() {
            let base = decide(&AttentionInput {
                novelty: Novelty::Unknown,
                ..input
            });
            let d = decide(&input);
            assert_eq!(d.phrase, base.phrase, "{input:?}");
            assert_eq!(d.reason, base.reason, "{input:?}");
            assert_eq!(d.episode, base.episode, "{input:?}");
            if d.tier == Tier::Critical {
                // Only because the class is critical, never because of the hint.
                assert_eq!(base.tier, Tier::Critical, "{input:?}");
                assert!(input.class.is_critical());
            }
            let radio_only_arrival = input.corroboration.count() <= 1
                && input.channel.modality() == Some(Modality::CarriedRadio);
            if !radio_only_arrival {
                // Novelty touches nothing but a radio-only arrival.
                assert_eq!(d, base, "{input:?}");
            }
            if input.novelty == Novelty::Familiar {
                // Familiarity may hold a radio-only arrival at digest, never
                // lower and never anything else.
                assert!(d.tier <= base.tier.max(Tier::Digest), "{input:?}");
            }
        }
    }

    #[test]
    fn unset_away_and_night_decide_identically_in_v0() {
        for input in every_input() {
            if input.posture != HouseholdPosture::Unset {
                continue;
            }
            let unset = decide(&input);
            for posture in [HouseholdPosture::Away, HouseholdPosture::Night] {
                assert_eq!(
                    decide(&AttentionInput { posture, ..input }),
                    unset,
                    "{input:?}"
                );
            }
        }
    }

    #[test]
    fn home_acts_only_on_indoor_witnesses_and_only_lowers() {
        for input in every_input() {
            if input.posture != HouseholdPosture::Home {
                continue;
            }
            let home = decide(&input);
            let unset = decide(&AttentionInput {
                posture: HouseholdPosture::Unset,
                ..input
            });
            // Posture acts on sensing, not on wording.
            assert_eq!(home.phrase, unset.phrase, "{input:?}");
            assert_eq!(home.episode, unset.episode, "{input:?}");
            assert!(home.tier <= unset.tier, "{input:?}");
            if input.zone_kind == ZoneKind::Indoor && !input.class.is_critical() {
                assert_eq!(home.tier, Tier::None, "{input:?}");
                let want = if input.witness_honors_posture {
                    ReasonCode::HomeIndoorAtWitness
                } else {
                    ReasonCode::HomeIndoor
                };
                assert_eq!(home.reason, want, "{input:?}");
            } else {
                // Outdoor, door, unknown zones and every critical event: no
                // effect at all.
                assert_eq!(home, unset, "{input:?}");
            }
        }
    }

    #[test]
    fn none_is_produced_only_by_home_on_an_indoor_witness() {
        for input in every_input() {
            if decide(&input).tier == Tier::None {
                assert_eq!(input.posture, HouseholdPosture::Home, "{input:?}");
                assert_eq!(input.zone_kind, ZoneKind::Indoor, "{input:?}");
                assert!(!input.class.is_critical(), "{input:?}");
            }
        }
    }

    #[test]
    fn a_single_weak_channel_never_interrupts_except_a_sustained_unfamiliar_radio_arrival() {
        for input in every_input() {
            let d = decide(&input);
            if input.corroboration.count() <= 1
                && input.channel.is_weak()
                && !input.class.is_critical()
                && d.tier > Tier::Digest
            {
                // The one exception the spec allows (rule 4 here; spec §7.2 rule 5).
                assert_eq!(d.reason, ReasonCode::RfOnlyArrival, "{input:?}");
                assert_eq!(input.novelty, Novelty::Unfamiliar, "{input:?}");
                assert_eq!(input.dwell, DwellBand::Sustained, "{input:?}");
            }
        }
    }

    #[test]
    fn the_hour_changes_nothing_in_v0() {
        for class in EventClass::ALL {
            for channel in Channel::ALL {
                for posture in HouseholdPosture::ALL {
                    let input = AttentionInput {
                        posture,
                        zone_kind: ZoneKind::Indoor,
                        ..base_input(class, channel)
                    };
                    let reference = decide(&input);
                    for hour_bucket in 0..=u8::MAX {
                        assert_eq!(
                            decide(&AttentionInput {
                                hour_bucket,
                                ..input
                            }),
                            reference
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn an_unknown_word_is_digest_at_most_never_critical() {
        for word in [
            "",
            "TAMPER",
            " tamper",
            "Tamper",
            "tamper_detected",
            "fire",
            "x",
        ] {
            assert_eq!(class_for_device_word(word), EventClass::Other, "{word:?}");
        }
        for input in every_input() {
            if input.class != EventClass::Other {
                continue;
            }
            let d = decide(&input);
            assert!(d.tier <= Tier::Digest, "{input:?}");
        }
    }

    // ------------------------------------------------------------ unit facts

    #[test]
    fn two_radios_on_one_phone_are_one_class() {
        assert_eq!(
            CorroborationClasses::from_channels([Channel::WifiRf, Channel::Ble]).count(),
            1
        );
        assert_eq!(
            CorroborationClasses::from_channels([Channel::Light, Channel::Vision]).count(),
            1
        );
        assert_eq!(
            CorroborationClasses::from_channels([Channel::Ble, Channel::Radar]).count(),
            2
        );
        // An unknown channel proves nothing.
        assert_eq!(
            CorroborationClasses::from_channels([Channel::Unknown, Channel::Vision]).count(),
            1
        );
        assert_eq!(CorroborationClasses::from_channels(Channel::ALL).count(), 7);
        assert_eq!(CorroborationClasses::from_count(200).count(), 7);
        assert_eq!(
            CorroborationClasses::default(),
            CorroborationClasses::UNKNOWN
        );
        // The radios together still read as an arrival on radio alone.
        let input = AttentionInput {
            corroboration: CorroborationClasses::from_channels([Channel::WifiRf, Channel::Ble]),
            novelty: Novelty::Familiar,
            dwell: DwellBand::Sustained,
            ..base_input(EventClass::Presence, Channel::Ble)
        };
        let d = decide(&input);
        assert_eq!(
            (d.tier, d.reason),
            (Tier::Digest, ReasonCode::RfOnlyArrival)
        );
    }

    #[test]
    fn dwell_bands_at_ninety_seconds() {
        assert_eq!(DwellBand::from_secs(0), DwellBand::None);
        assert_eq!(DwellBand::from_secs(1), DwellBand::Brief);
        assert_eq!(DwellBand::from_secs(89), DwellBand::Brief);
        assert_eq!(DwellBand::from_secs(90), DwellBand::Sustained);
        assert_eq!(DwellBand::from_secs(u64::MAX), DwellBand::Sustained);
    }

    #[test]
    fn household_posture_wire_bytes() {
        assert_eq!(HouseholdPosture::Unset.wire_byte(), None);
        assert_eq!(
            HouseholdPosture::from_wire_byte(0x00),
            Some(HouseholdPosture::Away)
        );
        assert_eq!(
            HouseholdPosture::from_wire_byte(0x01),
            Some(HouseholdPosture::Home)
        );
        assert_eq!(
            HouseholdPosture::from_wire_byte(0x02),
            Some(HouseholdPosture::Night)
        );
        for byte in 0x03..=u8::MAX {
            assert_eq!(HouseholdPosture::from_wire_byte(byte), None);
        }
    }

    #[test]
    fn ids_round_trip() {
        for v in HouseholdPosture::ALL {
            assert_eq!(HouseholdPosture::from_id(v.id()), Some(v));
        }
        for v in Tier::ALL {
            assert_eq!(Tier::from_id(v.id()), Some(v));
        }
        for v in ZoneKind::ALL {
            assert_eq!(ZoneKind::from_id(v.id()), Some(v));
        }
        for v in Modality::ALL {
            assert_eq!(Modality::from_id(v.id()), Some(v));
        }
        for v in Channel::ALL {
            assert_eq!(Channel::from_id(v.id()), Some(v));
        }
        for v in DwellBand::ALL {
            assert_eq!(DwellBand::from_id(v.id()), Some(v));
        }
        for v in Novelty::ALL {
            assert_eq!(Novelty::from_id(v.id()), Some(v));
        }
        for v in EventClass::ALL {
            assert_eq!(EventClass::from_id(v.id()), Some(v));
        }
        for v in EpisodeFamily::ALL {
            assert_eq!(EpisodeFamily::from_id(v.id()), Some(v));
        }
        for v in PhraseId::ALL {
            assert_eq!(PhraseId::from_id(v.id()), Some(v));
        }
        for v in ReasonCode::ALL {
            assert_eq!(ReasonCode::from_id(v.id()), Some(v));
        }
    }

    #[test]
    fn phrases_have_one_blank_and_no_identity_words() {
        const FORBIDDEN: &[&str] = &[
            "name",
            "known",
            "unknown",
            "stranger",
            "intruder",
            "recognized",
            "verified",
            "familiar",
            "unfamiliar",
            "authorized",
            "unauthorized",
            "household",
        ];
        let texts = PhraseId::ALL
            .iter()
            .map(|p| p.text())
            .chain(ReasonCode::ALL.iter().map(|r| r.text()));
        for text in texts {
            let without_blank = text.replace("{zone}", "");
            assert!(
                !without_blank.contains('{') && !without_blank.contains('}'),
                "{text:?} has a blank other than {{zone}}"
            );
            for word in without_blank
                .split(|c: char| !c.is_ascii_alphabetic())
                .filter(|w| !w.is_empty())
            {
                assert!(
                    !FORBIDDEN.contains(&word.to_ascii_lowercase().as_str()),
                    "{text:?} says {word:?}"
                );
            }
        }
    }

    #[test]
    fn cloud_class_returns_only_the_four_existing_classes() {
        let mut seen = std::collections::HashSet::new();
        for input in every_input() {
            if let Some(class) = cloud_class(&decide(&input)) {
                assert_ne!(class, PokeClass::Drill);
                seen.insert(class);
            }
        }
        assert_eq!(seen.len(), 4, "tamper, integrity, offline, pattern");
        assert!(seen.contains(&PokeClass::Tamper));
        assert!(seen.contains(&PokeClass::Integrity));
        assert!(seen.contains(&PokeClass::Offline));
        assert!(seen.contains(&PokeClass::Pattern));
    }

    #[test]
    fn the_class_map_matches_both_dialects_exhaustively() {
        // The kernel side is exhaustive by `match`; this pins the critical
        // rows, which are the ones that must never drift quietly.
        assert_eq!(
            class_for_event_type(&EventType::TamperDetected),
            EventClass::Tamper
        );
        for (word, class) in [
            ("tamper", EventClass::Tamper),
            ("panic", EventClass::Tamper),
            ("chain_break", EventClass::Integrity),
            ("verify_failed", EventClass::Integrity),
            ("witness_lost", EventClass::Dark),
            ("smoke_alarm_t3", EventClass::SmokeAlarmHeard),
            ("co_alarm_t4", EventClass::CoAlarmHeard),
        ] {
            assert_eq!(class_for_device_word(word), class, "{word}");
            assert!(class.is_critical(), "{word}");
        }
    }

    #[test]
    fn the_governor_ports_repeat_governor_numbers() {
        assert_eq!(EpisodeGovernor::cooldown_after(0), 0);
        assert_eq!(EpisodeGovernor::cooldown_after(1), 120);
        assert_eq!(EpisodeGovernor::cooldown_after(2), 240);
        assert_eq!(EpisodeGovernor::cooldown_after(3), 480);
        assert_eq!(EpisodeGovernor::cooldown_after(4), 960);
        assert_eq!(EpisodeGovernor::cooldown_after(5), 1800);
        assert_eq!(EpisodeGovernor::cooldown_after(17), 1800);
        assert_eq!(EpisodeGovernor::cooldown_after(u32::MAX), 1800);
    }

    #[test]
    fn forget_calm_drops_only_finished_episodes() {
        let mut g = EpisodeGovernor::default();
        let a = EpisodeKey {
            family: EpisodeFamily::Someone,
            zone: ZoneSlot(1),
        };
        let b = EpisodeKey {
            family: EpisodeFamily::Visitor,
            zone: ZoneSlot(1),
        };
        g.consider(a, Tier::Important, 0);
        g.consider(b, Tier::Digest, 1000);
        assert_eq!(g.len(), 2);
        g.forget_calm(1800);
        assert_eq!(g.len(), 1, "a has been calm 1800 s; b only 800 s");
        g.forget_calm(2800);
        assert!(g.is_empty());
    }

    // ------------------------------------------------- conformance vectors

    fn load_fixture() -> Value {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("spec")
            .join("fixtures")
            .join("attention_v1.json");
        let raw = std::fs::read_to_string(&path).unwrap_or_else(|e| {
            panic!(
                "could not read the attention vectors at {}: {e}\n\
                 They are the cross-surface contract for how loudly the hub \
                 notifies (spec/household_keys_v0.md §7). If they moved, \
                 update this path — never delete the check.",
                path.display()
            )
        });
        serde_json::from_str(&raw).expect("attention vectors are not valid JSON")
    }

    fn array<'a>(doc: &'a Value, key: &str) -> &'a Vec<Value> {
        let items = doc[key]
            .as_array()
            .unwrap_or_else(|| panic!("attention vectors have no `{key}` array"));
        assert!(
            !items.is_empty(),
            "attention vectors' `{key}` is empty — the check would pass vacuously"
        );
        items
    }

    /// The object at `v[key]`, refusing any key outside `allowed` and
    /// requiring every one of them: a misspelled field must fail, not
    /// silently default.
    fn strict<'a>(
        v: &'a Value,
        name: &str,
        allowed: &[&str],
    ) -> &'a serde_json::Map<String, Value> {
        let obj = v
            .as_object()
            .unwrap_or_else(|| panic!("{name}: not an object"));
        for key in obj.keys() {
            assert!(
                allowed.contains(&key.as_str()),
                "{name}: unexpected key {key:?}"
            );
        }
        for key in allowed {
            assert!(obj.contains_key(*key), "{name}: missing key {key:?}");
        }
        obj
    }

    fn id<'a>(obj: &'a serde_json::Map<String, Value>, name: &str, key: &str) -> &'a str {
        obj[key]
            .as_str()
            .unwrap_or_else(|| panic!("{name}: `{key}` is not a string"))
    }

    fn byte(obj: &serde_json::Map<String, Value>, name: &str, key: &str) -> u8 {
        obj[key]
            .as_u64()
            .and_then(|n| u8::try_from(n).ok())
            .unwrap_or_else(|| panic!("{name}: `{key}` is not a byte"))
    }

    fn flag(obj: &serde_json::Map<String, Value>, name: &str, key: &str) -> bool {
        obj[key]
            .as_bool()
            .unwrap_or_else(|| panic!("{name}: `{key}` is not a bool"))
    }

    fn parse<T>(found: Option<T>, name: &str, what: &str, raw: &str) -> T {
        found.unwrap_or_else(|| panic!("{name}: {raw:?} is not a dictionary {what} id"))
    }

    fn parse_input(v: &Value, name: &str) -> AttentionInput {
        let o = strict(
            v,
            name,
            &[
                "class",
                "zone",
                "zone_kind",
                "posture",
                "witness_honors_posture",
                "channel",
                "corroboration",
                "dwell",
                "novelty",
                "hour_bucket",
                "quiet_hours",
            ],
        );
        let corroboration = o["corroboration"]
            .as_array()
            .unwrap_or_else(|| panic!("{name}: `corroboration` is not a list of channel ids"))
            .iter()
            .map(|c| {
                let raw = c.as_str().expect("channel id");
                parse(Channel::from_id(raw), name, "channel", raw)
            })
            .collect::<Vec<_>>();
        AttentionInput {
            class: parse(
                EventClass::from_id(id(o, name, "class")),
                name,
                "class",
                id(o, name, "class"),
            ),
            zone: ZoneSlot(byte(o, name, "zone")),
            zone_kind: parse(
                ZoneKind::from_id(id(o, name, "zone_kind")),
                name,
                "zone kind",
                id(o, name, "zone_kind"),
            ),
            posture: parse(
                HouseholdPosture::from_id(id(o, name, "posture")),
                name,
                "household posture",
                id(o, name, "posture"),
            ),
            witness_honors_posture: flag(o, name, "witness_honors_posture"),
            channel: parse(
                Channel::from_id(id(o, name, "channel")),
                name,
                "channel",
                id(o, name, "channel"),
            ),
            corroboration: CorroborationClasses::from_channels(corroboration),
            dwell: parse(
                DwellBand::from_id(id(o, name, "dwell")),
                name,
                "dwell band",
                id(o, name, "dwell"),
            ),
            novelty: parse(
                Novelty::from_id(id(o, name, "novelty")),
                name,
                "novelty",
                id(o, name, "novelty"),
            ),
            hour_bucket: byte(o, name, "hour_bucket"),
            quiet_hours: flag(o, name, "quiet_hours"),
        }
    }

    fn parse_episode_key(v: &Value, name: &str) -> EpisodeKey {
        let o = strict(v, name, &["family", "zone"]);
        EpisodeKey {
            family: parse(
                EpisodeFamily::from_id(id(o, name, "family")),
                name,
                "episode family",
                id(o, name, "family"),
            ),
            zone: ZoneSlot(byte(o, name, "zone")),
        }
    }

    #[test]
    fn every_attention_vector_decides_as_pinned() {
        let doc = load_fixture();
        let vectors = array(&doc, "vectors");
        assert!(
            vectors.len() >= 40,
            "the vector set should cover every rule"
        );
        let mut names = std::collections::HashSet::new();
        for v in vectors {
            let o = strict(v, "vector", &["name", "input", "expect"]);
            let name = id(o, "vector", "name");
            assert!(
                names.insert(name.to_string()),
                "duplicate vector name {name:?}"
            );
            let input = parse_input(&o["input"], name);
            let e = strict(
                &o["expect"],
                name,
                &["tier", "phrase", "reason", "held_for_morning", "episode"],
            );
            let want = AttentionDecision {
                tier: parse(
                    Tier::from_id(id(e, name, "tier")),
                    name,
                    "tier",
                    id(e, name, "tier"),
                ),
                phrase: parse(
                    PhraseId::from_id(id(e, name, "phrase")),
                    name,
                    "phrase",
                    id(e, name, "phrase"),
                ),
                reason: parse(
                    ReasonCode::from_id(id(e, name, "reason")),
                    name,
                    "reason",
                    id(e, name, "reason"),
                ),
                held_for_morning: flag(e, name, "held_for_morning"),
                episode: parse_episode_key(&e["episode"], name),
            };
            assert_eq!(decide(&input), want, "vector {name:?}");
        }
    }

    #[test]
    fn every_word_vector_maps_to_its_class() {
        let doc = load_fixture();
        for v in array(&doc, "words") {
            let o = v.as_object().expect("word vector is an object");
            let class_raw = o["class"].as_str().expect("word vector has a class");
            let want = parse(
                EventClass::from_id(class_raw),
                "word vector",
                "class",
                class_raw,
            );
            match (o.get("device_word"), o.get("kernel_event_type")) {
                (Some(word), None) => {
                    let word = word.as_str().expect("device_word is a string");
                    assert_eq!(class_for_device_word(word), want, "device word {word:?}");
                }
                (None, Some(kernel)) => {
                    let event: EventType = serde_json::from_value(kernel.clone())
                        .unwrap_or_else(|e| panic!("kernel_event_type {kernel}: {e}"));
                    assert_eq!(class_for_event_type(&event), want, "kernel event {kernel}");
                }
                _ => {
                    panic!("word vector needs exactly one of device_word / kernel_event_type: {v}")
                }
            }
        }
    }

    #[test]
    fn every_episode_sequence_governs_as_pinned() {
        let doc = load_fixture();
        for seq in array(&doc, "episodes") {
            let o = strict(seq, "episode sequence", &["name", "steps"]);
            let name = id(o, "episode sequence", "name");
            let steps = o["steps"]
                .as_array()
                .unwrap_or_else(|| panic!("{name}: no steps"));
            assert!(!steps.is_empty(), "{name}: no steps");
            let mut governor = EpisodeGovernor::default();
            for (i, step) in steps.iter().enumerate() {
                let label = format!("{name} step {i}");
                let s = strict(step, &label, &["at", "tier", "episode", "expect"]);
                let at = s["at"]
                    .as_u64()
                    .unwrap_or_else(|| panic!("{label}: `at` is not seconds"));
                let tier = parse(
                    Tier::from_id(id(s, &label, "tier")),
                    &label,
                    "tier",
                    id(s, &label, "tier"),
                );
                let key = parse_episode_key(&s["episode"], &label);
                let e = s["expect"]
                    .as_object()
                    .unwrap_or_else(|| panic!("{label}: no expect"));
                let want = match e["verdict"].as_str() {
                    Some("notify") => {
                        strict(&s["expect"], &label, &["verdict", "cause"]);
                        EpisodeVerdict::Notify(match e["cause"].as_str() {
                            Some("first_of_episode") => NotifyCause::FirstOfEpisode,
                            Some("escalation") => NotifyCause::Escalation,
                            Some("critical") => NotifyCause::Critical,
                            Some("rest_elapsed") => NotifyCause::RestElapsed,
                            other => panic!("{label}: unknown cause {other:?}"),
                        })
                    }
                    Some("collapsed") => {
                        strict(&s["expect"], &label, &["verdict", "resting_for_secs"]);
                        EpisodeVerdict::Collapsed {
                            resting_for_secs: e["resting_for_secs"]
                                .as_u64()
                                .unwrap_or_else(|| panic!("{label}: resting_for_secs")),
                        }
                    }
                    Some("not_surfaced") => {
                        strict(&s["expect"], &label, &["verdict"]);
                        EpisodeVerdict::NotSurfaced
                    }
                    other => panic!("{label}: unknown verdict {other:?}"),
                };
                assert_eq!(governor.consider(key, tier, at), want, "{label}");
            }
        }
    }
}
