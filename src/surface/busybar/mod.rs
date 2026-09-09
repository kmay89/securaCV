//! The BUSY Bar surface — a two-audience witness readout on someone else's
//! hardware.
//!
//! A [BUSY Bar](https://busy.app) (Flipper Devices) is a desk or door object
//! with a 72x16 RGB LED matrix on the front, a 160x80 monochrome display on
//! the back, a center button, a rotary dial, a mode switch, a top lever, and
//! an open local HTTP API. This module drives one from the hub as a SecuraCV
//! **display surface**.
//!
//! It is not a Canary, and it is not called one. Canaries are SecuraCV's own
//! witnessing devices (`docs/GLOSSARY.md`); this is third-party hardware
//! rendering what the Canaries already witnessed, so it gets an honest name —
//! the BUSY Bar surface, `busybar` in code — and BUSY Bar is referred to as
//! what it is, someone else's product (`TRADEMARK.md` §1, truthful
//! compatibility statements).
//!
//! # The design in one idea
//!
//! **The device's two displays map onto the Canary Cards privacy classing.**
//!
//! - The **front matrix is room-facing, and therefore public.** Anyone at the
//!   door reads it. It renders public-class content only, through a closed
//!   phrase vocabulary ([`phrase`]) that no network payload can extend.
//! - The **rear display is operator-facing** — you have to be at the device.
//!   It carries the detail: chain status, per-Canary liveness with last-seen,
//!   the recent verified count, the current mode, and which transport this
//!   surface is using, so the trust boundary is legible at arm's length.
//!
//! Both are derived from the same card set the device already announces to
//! Home Assistant, through the classing gate in [`card`]. There is no second
//! formatting path, and **a card that cannot be classed is not rendered.
//! Ever.**
//!
//! # Module map
//!
//! | module | what it holds |
//! |---|---|
//! | [`phrase`] | the closed public vocabulary; why PII on the front is a `can't` |
//! | [`card`] | Canary Cards in Rust, and the front/rear classing gate |
//! | [`ingest`] | the wire-to-cards projection, mirroring the Lab renderer |
//! | [`state`] | the state machine, the breath, the dead-man's switch |
//! | [`controls`] | the controls, and why none of them can affect witnessing |
//! | [`device`] | the HTTP protocol mapping, as a pure function |
//!
//! Everything above is pure: no sockets, no clocks. The daemon that owns
//! both is `src/bin/busybar_surface.rs`.
//!
//! # Status
//!
//! **Bench validation pending.** Nothing here has been exercised against a
//! real BUSY Bar. The protocol shapes come from the firmware's own OpenAPI
//! specification and the two official client libraries, which is good
//! evidence and not the same thing as a device on a desk. The physical
//! controls are designed and unimplemented for reasons given in
//! [`controls`]. `docs/design/busybar_surface.md` §9 is the list of what a
//! bench session has to settle, and the README claims nothing else.

pub mod card;
pub mod controls;
pub mod ingest;
pub mod device;
pub mod phrase;
pub mod state;

use std::collections::BTreeMap;

use phrase::{ZoneWord, ZoneWordError};

/// The operator's zone table: `zone_id` to the one word the front matrix may
/// carry for it.
///
/// Built once, at config load, from the surface's own TOML file. Nothing on
/// the wire can add an entry, which is the whole reason the front's variable
/// vocabulary is safe (see [`phrase`]).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ZoneTable {
    labels: BTreeMap<String, ZoneWord>,
}

/// A zone table entry that did not pass [`ZoneWord::try_new`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ZoneTableError {
    /// The `zone_id` whose label was refused.
    pub zone_id: String,
    /// Why.
    pub error: ZoneWordError,
}

impl std::fmt::Display for ZoneTableError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "zone \"{}\": {}", self.zone_id, self.error)
    }
}

impl std::error::Error for ZoneTableError {}

/// Most zones an operator may label. A bound (FR-4), and a generous one.
pub const MAX_ZONES: usize = 64;

impl ZoneTable {
    /// Build from `(zone_id, label)` pairs, validating every label.
    ///
    /// Fails on the **first** bad label rather than dropping it. A silently
    /// dropped zone label is a zone that renders unlabeled forever and looks
    /// like a bug in the wrong place; a config error at startup is a message
    /// the operator can act on.
    pub fn build<I, K, V>(entries: I) -> Result<Self, ZoneTableError>
    where
        I: IntoIterator<Item = (K, V)>,
        K: AsRef<str>,
        V: AsRef<str>,
    {
        let mut labels = BTreeMap::new();
        for (zone_id, label) in entries.into_iter().take(MAX_ZONES) {
            let zone_id = normalize_zone_id(zone_id.as_ref());
            let word = ZoneWord::try_new(label.as_ref()).map_err(|error| ZoneTableError {
                zone_id: zone_id.clone(),
                error,
            })?;
            labels.insert(zone_id, word);
        }
        Ok(ZoneTable { labels })
    }

    /// The label for a zone, or `None` when the operator has not named it.
    ///
    /// `None` is the safe answer and the common one: the presence state still
    /// shows, the place stays unnamed. A zone that turns up on the wire and
    /// is not in this table cannot introduce a word.
    pub fn label(&self, zone_id: &str) -> Option<&ZoneWord> {
        self.labels.get(&normalize_zone_id(zone_id))
    }

    /// How many zones are labeled.
    pub fn len(&self) -> usize {
        self.labels.len()
    }

    /// True when no zone is labeled.
    pub fn is_empty(&self) -> bool {
        self.labels.is_empty()
    }
}

/// Reduce a zone id to the form the kernel uses.
///
/// The kernel stamps `zone:<sanitized>` onto every event
/// (`crate::adapter::contract`), and an operator writing a config file should
/// not have to remember the prefix. Both forms land on the same key.
fn normalize_zone_id(raw: &str) -> String {
    raw.trim()
        .trim_start_matches("zone:")
        .trim()
        .to_ascii_lowercase()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_table_accepts_a_zone_with_or_without_the_kernel_prefix() {
        let t = ZoneTable::build([("front_door", "front door")]).expect("table");
        assert_eq!(t.label("front_door").map(ZoneWord::as_str), Some("FRONT DOOR"));
        assert_eq!(
            t.label("zone:front_door").map(ZoneWord::as_str),
            Some("FRONT DOOR")
        );
        assert_eq!(t.label("FRONT_DOOR").map(ZoneWord::as_str), Some("FRONT DOOR"));
    }

    /// The safe default: a zone nobody named renders with no label. The
    /// presence state still shows; the place stays unnamed. This is what
    /// stops a payload naming an unconfigured zone from introducing a word.
    #[test]
    fn an_unlabeled_zone_has_no_word() {
        let t = ZoneTable::build([("front_door", "FRONT DOOR")]).expect("table");
        assert!(t.label("bedroom").is_none());
        assert!(t.label("zone:whatever_the_payload_said").is_none());
    }

    /// A bad label fails at startup with a message naming the zone, rather
    /// than being dropped into a zone that renders unlabeled forever and
    /// looks like a bug somewhere else.
    #[test]
    fn a_bad_label_fails_the_config_rather_than_being_dropped() {
        let err = ZoneTable::build([("hall", "K. AT DOOR")]).expect_err("should refuse");
        assert_eq!(err.zone_id, "hall");
        let rendered = err.to_string();
        assert!(rendered.contains("hall"), "{rendered}");
        assert!(rendered.contains("K. AT DOOR"), "{rendered}");
    }

    #[test]
    fn the_table_is_bounded() {
        let many: Vec<(String, String)> = (0..MAX_ZONES + 50)
            .map(|i| (format!("zone{i}"), format!("ZONE {i:03}")))
            .collect();
        let t = ZoneTable::build(many).expect("table");
        assert!(t.len() <= MAX_ZONES);
    }

    #[test]
    fn an_empty_table_is_valid_and_labels_nothing() {
        let t = ZoneTable::default();
        assert!(t.is_empty());
        assert!(t.label("front_door").is_none());
    }
}
