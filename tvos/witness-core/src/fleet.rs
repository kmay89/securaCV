//! The `GET /api/fleet` contract, parsed once in Rust so the TV, the emulator,
//! and any future client agree on what a fleet *is*.
//!
//! The contract (and the browser rules that decide where it can work) is
//! `tvos/discovery/DISCOVERY.md`. Two rules from it are load-bearing and
//! enforced by the tests below:
//!
//! * only `devices[].name` is required; `online` defaults to `false` — a
//!   silent field is never a presence claim — and `chain` / `product` / `hw`
//!   / `hub`, plus the coarse wellbeing keys (`presence` / `occupants` /
//!   `breathing` / `seeing` / `seeing_score`), are shown only when present.
//!   That default is pinned by
//!   `tests/fixtures/fleet_contract_vectors.json` (`tests/fleet_contract.rs`),
//!   because it once drifted three ways between the doc, this crate and the
//!   Swift decoder;
//! * a bare JSON array of devices is also accepted;
//! * `chain: "unknown"` is an ABSENT claim, not a failure — a display holds no
//!   witness chain of its own, and painting it as trouble is a lie about every
//!   display in the fleet;
//! * `born_day` / `born_exact` (when the device's key was born) are carried
//!   only when they are a day and a flag the firmware could have written, and
//!   a malformed one is dropped rather than failing the snapshot: one strange
//!   row must not blank the television for every device that answered.
//!
//! This is coarse presence and health — never secrets, never media.

use serde::{Deserialize, Deserializer, Serialize};
use serde_json::Value;

/// A device that omits `online` is NOT claimed present. The phone's decoder
/// and the Wall's Swift `Device` already defaulted to false ("absent is not a
/// presence claim — never rendered as online"); this normalizer said true, so
/// the two halves of the same app disagreed about the same silent field.
fn default_online() -> bool {
    false
}

/// The first day a Canary can call its birth day:
/// `kClockFloor / kSecondsPerDay` in `firmware/common/identity/birth_day.h`
/// (2025-01-01). The firmware never stamps a clock reading below that floor
/// ("the epoch showing through, not a date", its rule 2), so a smaller day on
/// the wire is not a day any Canary stamped. `tests/fleet_contract.rs` reads
/// the header and holds this to it.
pub const BORN_DAY_MIN: u32 = 1_735_689_600 / 86_400;

/// The last day a 32-bit Unix clock can name (2106-02-07). The firmware's day
/// is `unix_s / 86400` of a `uint32_t` clock, so no Canary can stamp a later
/// one.
pub const BORN_DAY_MAX: u32 = u32::MAX / 86_400;

/// Keep `born_day` only when it is a day the firmware could have written: a
/// JSON number with an integral value inside
/// [`BORN_DAY_MIN`]`..=`[`BORN_DAY_MAX`]. Anything else (the firmware's own
/// "never dated" 0, a negative, a fraction, a string, a day past the u32
/// clock) is dropped to `None`, which every reader renders as "cannot say".
///
/// Dropped, never an error: serde fails the WHOLE snapshot on one wrong type,
/// and the field is optional and newer than most of the fleet. An integral
/// float (`20673.0`) is accepted because a JavaScript reader cannot tell it
/// from `20673` after `JSON.parse`, and the readers must agree.
fn tolerant_born_day<'de, D: Deserializer<'de>>(de: D) -> Result<Option<u32>, D::Error> {
    let value = Value::deserialize(de)?;
    let Value::Number(n) = value else {
        return Ok(None);
    };
    let day = match (n.as_u64(), n.as_f64()) {
        (Some(whole), _) => whole,
        (None, Some(f)) if f.fract() == 0.0 && f >= 0.0 && f <= f64::from(BORN_DAY_MAX) => f as u64,
        _ => return Ok(None),
    };
    Ok(u32::try_from(day)
        .ok()
        .filter(|d| (BORN_DAY_MIN..=BORN_DAY_MAX).contains(d)))
}

/// Keep `born_exact` only when it is a JSON boolean; anything else is dropped,
/// and a reader takes a missing flag as NOT exact.
fn tolerant_bool<'de, D: Deserializer<'de>>(de: D) -> Result<Option<bool>, D::Error> {
    Ok(match Value::deserialize(de)? {
        Value::Bool(b) => Some(b),
        _ => None,
    })
}

/// One Canary, as the fleet endpoint describes it.
#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
pub struct Device {
    pub name: String,
    #[serde(default = "default_online")]
    pub online: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chain: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub product: Option<String>,
    /// WHICH BOARD this is — the `boards/<id>/pins` header the device compiled
    /// against, as its pins header spells `CANARY_FIGURE_HARDWARE`.
    ///
    /// Carried through rather than dropped because it is the only field that
    /// is exact about the device's SHAPE: several products share one `product`
    /// string, so a client drawing from the product alone gets a coin flip or
    /// nothing. Dropping it here is what left the Wall showing a colored dot
    /// where the phone shows the device.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hw: Option<String>,
    /// Where the device stands with its hub: "none" (nobody configured one),
    /// "down" (configured, unreachable), "ok". Absent means it did not say,
    /// which a client must never render as "fine".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hub: Option<String>,
    /// THE ROOM, COARSELY — the sense line's presence story in the wire's own
    /// words: "clear" or "present". Kept verbatim on purpose (the `hub`
    /// rule): tolerance lives in the reader, so a word this normalizer has
    /// never met crosses intact and the client folds it to "cannot say"
    /// instead of a guess. Absent means the row did not say — which a client
    /// must never render as an empty calm room. This struct used to end at
    /// `hub`, and serde silently STRIPPED these keys before Swift ever saw
    /// them.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence: Option<String>,
    /// The headcount bucket, as words: "0" | "1" | "2+". Same verbatim rule.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub occupants: Option<String>,
    /// Breathing lock held (`true`) or lapsed (`false`); absent = the row is
    /// not claiming either.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub breathing: Option<bool>,
    /// What the camera line reports seeing — "person" / "vehicle" / "animal"
    /// / "package" today, carried verbatim so tomorrow's word still crosses.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seeing: Option<String>,
    /// Detection confidence, 0–100: the firmware writes it only beside a
    /// `seeing` word. Carried as sent — the reader decides what counts as a
    /// percentage, exactly as it decides what counts as a known word.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seeing_score: Option<i64>,
    /// When this device's KEY was born, in days since the Unix epoch (a UTC
    /// day, no time of day): `fleet_selfreport.h`'s `born_day`. A fact about
    /// the Canary, not about whoever paired it. `None` when the device did not
    /// say (it has never met a believable clock, or has no witness key of its
    /// own, like a display) and when it said something no firmware writes
    /// (see `tolerant_born_day`).
    #[serde(
        default,
        deserialize_with = "tolerant_born_day",
        skip_serializing_if = "Option::is_none"
    )]
    pub born_day: Option<u32>,
    /// `true` means `born_day` really is the day the key was born. `false`
    /// means it is the day the device was FIRST DATED (it met a clock later
    /// than it was made), and a reader must not call it a birthday. Carried
    /// as sent when it is a boolean, and independently of the day: the reader
    /// decides, and it reads a missing flag as not exact and a flag with no
    /// day as nothing at all.
    #[serde(
        default,
        deserialize_with = "tolerant_bool",
        skip_serializing_if = "Option::is_none"
    )]
    pub born_exact: Option<bool>,
}

impl Device {
    /// Does this device's own report describe a chain FAILURE?
    ///
    /// Three answers, not two. `ok` verifies. A missing field, or the explicit
    /// "unknown", both mean the device is not making a claim — a display has
    /// no chain of its own to report — and neither may be drawn as trouble.
    /// Anything else is a state the device is reporting as wrong, and is.
    pub fn chain_is_troubled(&self) -> bool {
        !matches!(self.chain.as_deref(), None | Some("ok") | Some("unknown"))
    }
}

/// A fleet snapshot. `kernel` and `verified_through` are optional so a single
/// Canary fronting itself can answer without pretending to be a hub.
#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
pub struct FleetSnapshot {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kernel: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub verified_through: Option<String>,
    #[serde(default)]
    pub devices: Vec<Device>,
}

impl FleetSnapshot {
    pub fn online_count(&self) -> usize {
        self.devices.iter().filter(|d| d.online).count()
    }

    /// True when any device reports a chain state that is a real FAILURE. The
    /// Wall surfaces this loudly — a fleet is not "fine" because most of it is.
    ///
    /// "unknown" is NOT a failure, and reading it as one was a lie the Wall
    /// told about every display in the fleet. A display holds no witness chain
    /// of its own — it renders other devices' — so it honestly answers
    /// "unknown", and the old `!= "ok"` test painted it orange with "Record
    /// didn't verify" and counted it as needing attention. Absence of a chain
    /// is not a broken chain, exactly as an absent `chain` field is not.
    pub fn has_chain_trouble(&self) -> bool {
        self.devices.iter().any(Device::chain_is_troubled)
    }

    /// The one-line summary the Wall puts under the fleet name. Never a group
    /// noun we don't use (CLAUDE.md): it's a fleet, or it's "your Canaries".
    pub fn summary(&self) -> String {
        let total = self.devices.len();
        if total == 0 {
            return "No Canaries reachable yet.".to_string();
        }
        let online = self.online_count();
        let noun = if total == 1 { "Canary" } else { "Canaries" };
        if online == total {
            format!("{total} {noun}, all online")
        } else {
            format!("{online} of {total} {noun} online")
        }
    }
}

/// Parse a fleet response, accepting either the documented object or the bare
/// array form.
pub fn parse_fleet(json: &str) -> Result<FleetSnapshot, String> {
    // Try the object shape first; fall back to the bare-array shape. Reporting
    // the *object* error is the more useful message when both fail, because
    // that is the documented shape.
    match serde_json::from_str::<FleetSnapshot>(json) {
        Ok(snapshot) => Ok(snapshot),
        Err(object_err) => match serde_json::from_str::<Vec<Device>>(json) {
            Ok(devices) => Ok(FleetSnapshot {
                kernel: None,
                verified_through: None,
                devices,
            }),
            Err(_) => Err(format!("not a fleet response: {object_err}")),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The exact example from tvos/discovery/DISCOVERY.md. If the doc changes,
    /// this test is the thing that notices.
    const DOC_EXAMPLE: &str = r#"{
      "kernel": "kitchen-hub",
      "verified_through": "4:02 PM",
      "devices": [
        { "name": "Front Door", "online": true,  "chain": "ok", "product": "canary-wap", "hub": "ok" },
        { "name": "Studio",     "online": true,  "chain": "ok", "product": "canary" },
        { "name": "Driveway",   "online": false, "chain": "ok", "product": "canary-vision", "hw": "xiao-esp32c3" }
      ]
    }"#;

    #[test]
    fn the_documented_example_parses() {
        let fleet = parse_fleet(DOC_EXAMPLE).expect("doc example must parse");
        assert_eq!(fleet.kernel.as_deref(), Some("kitchen-hub"));
        assert_eq!(fleet.devices.len(), 3);
        assert_eq!(fleet.online_count(), 2);
        assert!(!fleet.has_chain_trouble());
        assert_eq!(fleet.summary(), "2 of 3 Canaries online");
        // The doc's optional fields survive the parse: the board id that
        // resolves a figure, and the hub standing.
        assert_eq!(fleet.devices[0].hub.as_deref(), Some("ok"));
        assert_eq!(fleet.devices[2].hw.as_deref(), Some("xiao-esp32c3"));
        assert_eq!(fleet.devices[1].hw, None);
    }

    #[test]
    fn only_name_is_required_and_online_defaults_false() {
        // A silent field is never a presence claim — same rule as the Swift
        // decoder and the phone (DeviceParityTests.testASilentFieldCostsTheDeviceNothing).
        let fleet = parse_fleet(r#"{"devices":[{"name":"Porch"}]}"#).unwrap();
        assert!(!fleet.devices[0].online, "online must default to false");
        assert_eq!(fleet.devices[0].chain, None);
        assert!(fleet.summary().starts_with("0 of 1"), "{}", fleet.summary());
    }

    /// The bytes a display actually sends, verbatim from
    /// `fleet_selfreport_build()` in firmware/common/fleet_selfreport — the
    /// same literal the iPhone's FleetSelfReportTests pins. If the Wall and
    /// the phone ever disagree about a device, it starts here.
    const DISPLAY_SELFREPORT: &str = r#"{"kernel":"canary_nightstand7_001","verified_through":"now","devices":[{"name":"canary_nightstand7_001","online":true,"chain":"unknown","product":"canary-nightstand7","hw":"waveshare-esp32s3-lcd7","hub":"none"}]}"#;

    #[test]
    fn the_board_and_hub_survive_the_normalizer() {
        // These two were dropped on the floor here, which is why the Wall drew
        // a colored dot where the phone drew the device: a client cannot
        // resolve a figure it was never handed the board id for.
        let fleet = parse_fleet(DISPLAY_SELFREPORT).expect("a display's own bytes must parse");
        let d = &fleet.devices[0];
        assert_eq!(d.product.as_deref(), Some("canary-nightstand7"));
        assert_eq!(d.hw.as_deref(), Some("waveshare-esp32s3-lcd7"));
        assert_eq!(d.hub.as_deref(), Some("none"));
        // A display holds no witness chain of its own and says so. That is NOT
        // trouble — reading it as trouble painted every display on the Wall
        // orange with "Record didn't verify".
        assert_eq!(d.chain.as_deref(), Some("unknown"));
        assert!(
            !d.chain_is_troubled(),
            "\"unknown\" is an absent claim, not a failure"
        );
        assert!(!fleet.has_chain_trouble());
    }

    #[test]
    fn the_wellbeing_keys_survive_the_normalizer() {
        // This struct used to end at `hub`, and serde strips unknown fields —
        // so the wave-6 wellbeing keys vanished here before Swift ever saw
        // them. Words stay verbatim (tolerance lives in the reader), and a
        // row that says nothing stays silent: absent is "cannot say", never
        // an empty calm room. The full aggregated-body literal is pinned in
        // tests/fixtures/fleet_contract_vectors.json.
        let fleet = parse_fleet(
            r#"{"devices":[
                {"name":"Bedroom","online":true,"presence":"present","occupants":"1","breathing":true},
                {"name":"Driveway","online":true,"seeing":"package","seeing_score":87},
                {"name":"Porch"}]}"#,
        )
        .unwrap();
        assert_eq!(fleet.devices[0].presence.as_deref(), Some("present"));
        assert_eq!(fleet.devices[0].occupants.as_deref(), Some("1"));
        assert_eq!(fleet.devices[0].breathing, Some(true));
        assert_eq!(fleet.devices[1].seeing.as_deref(), Some("package"));
        assert_eq!(fleet.devices[1].seeing_score, Some(87));
        let silent = &fleet.devices[2];
        assert_eq!(silent.presence, None);
        assert_eq!(silent.occupants, None);
        assert_eq!(silent.breathing, None);
        assert_eq!(silent.seeing, None);
        assert_eq!(silent.seeing_score, None);
    }

    #[test]
    fn a_device_that_omits_the_new_fields_still_parses() {
        // Every device on firmware older than these fields, which is all of
        // them until the fleet takes an update. Absent must stay absent —
        // never "" and never a default that reads as an answer.
        let fleet =
            parse_fleet(r#"{"devices":[{"name":"Porch","product":"canary-wap"}]}"#).unwrap();
        assert_eq!(fleet.devices[0].hw, None);
        assert_eq!(fleet.devices[0].hub, None);
    }

    /// A day and its exactness, verbatim from `fleet_selfreport_build()` (the
    /// same bytes the iPhone's FleetSelfReportTests.bornBody pins).
    #[test]
    fn the_birth_day_survives_the_normalizer() {
        let fleet = parse_fleet(
            r#"{"devices":[{"name":"Front Door","online":true,"chain":"ok","product":"canary-wap","chain_height":42,"born_day":20673,"born_exact":true}]}"#,
        )
        .unwrap();
        assert_eq!(fleet.devices[0].born_day, Some(20673));
        assert_eq!(fleet.devices[0].born_exact, Some(true));
    }

    #[test]
    fn a_birth_day_no_firmware_writes_is_dropped_not_fatal() {
        // One strange row must not cost the snapshot: serde would fail the
        // whole parse on a wrong type, and a failed parse blanks the Wall for
        // every device that answered. Each of these is "cannot say".
        for day in [
            "0",
            "-1",
            "20088",
            "49711",
            "4294967296",
            "20673.5",
            "\"20673\"",
            "true",
            "null",
            "[20673]",
            "{\"d\":20673}",
        ] {
            let body = format!(r#"{{"devices":[{{"name":"A","born_day":{day}}},{{"name":"B"}}]}}"#);
            let fleet = parse_fleet(&body).unwrap_or_else(|e| panic!("born_day {day}: {e}"));
            assert_eq!(fleet.devices.len(), 2, "born_day {day} cost a device");
            assert_eq!(
                fleet.devices[0].born_day, None,
                "born_day {day} became a day"
            );
        }
        // The edges of what a Canary can stamp are days, and an integral
        // float is the same day to every reader (JSON.parse cannot tell).
        for (raw, want) in [("20089", 20089), ("49710", 49710), ("20673.0", 20673)] {
            let body = format!(r#"{{"devices":[{{"name":"A","born_day":{raw}}}]}}"#);
            assert_eq!(
                parse_fleet(&body).unwrap().devices[0].born_day,
                Some(want),
                "{raw}"
            );
        }
        for flag in ["1", "\"true\"", "null", "0"] {
            let body =
                format!(r#"{{"devices":[{{"name":"A","born_day":20673,"born_exact":{flag}}}]}}"#);
            let d = &parse_fleet(&body).unwrap().devices[0];
            assert_eq!(d.born_day, Some(20673));
            assert_eq!(d.born_exact, None, "born_exact {flag} is not a boolean");
        }
    }

    #[test]
    fn half_a_birth_pair_crosses_as_sent() {
        // The firmware writes the two together, but the reader is the one that
        // decides what half a pair means (a day alone is not exact; a flag
        // alone qualifies nothing), so neither is invented or dropped here.
        let fleet = parse_fleet(
            r#"{"devices":[{"name":"A","born_day":20673},{"name":"B","born_exact":true},{"name":"C"}]}"#,
        )
        .unwrap();
        assert_eq!(fleet.devices[0].born_day, Some(20673));
        assert_eq!(fleet.devices[0].born_exact, None);
        assert_eq!(fleet.devices[1].born_day, None);
        assert_eq!(fleet.devices[1].born_exact, Some(true));
        assert_eq!(fleet.devices[2].born_day, None);
        assert_eq!(fleet.devices[2].born_exact, None);
        // And absent stays absent on the way out to Swift: no key, not a null.
        let out = serde_json::to_string(&fleet.devices[2]).unwrap();
        assert!(!out.contains("born"), "{out}");
    }

    #[test]
    fn a_bare_array_of_devices_is_accepted() {
        let fleet = parse_fleet(r#"[{"name":"Front Door"},{"name":"Studio"}]"#).unwrap();
        assert_eq!(fleet.devices.len(), 2);
        assert_eq!(fleet.summary(), "0 of 2 Canaries online");
    }

    #[test]
    fn an_empty_fleet_says_so_plainly() {
        let fleet = parse_fleet(r#"{"devices":[]}"#).unwrap();
        assert_eq!(fleet.summary(), "No Canaries reachable yet.");
    }

    #[test]
    fn a_bad_chain_state_is_surfaced() {
        let fleet =
            parse_fleet(r#"{"devices":[{"name":"A","chain":"ok"},{"name":"B","chain":"broken"}]}"#)
                .unwrap();
        assert!(fleet.has_chain_trouble());
    }

    #[test]
    fn junk_is_an_error_not_a_panic() {
        assert!(parse_fleet("<html>nope</html>").is_err());
    }
}
