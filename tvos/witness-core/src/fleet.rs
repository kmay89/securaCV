//! The `GET /api/fleet` contract, parsed once in Rust so the TV, the emulator,
//! and any future client agree on what a fleet *is*.
//!
//! The contract (and the browser rules that decide where it can work) is
//! `tvos/discovery/DISCOVERY.md`. Two rules from it are load-bearing and
//! enforced by the tests below:
//!
//! * only `devices[].name` is required; `online` defaults to `true`, and
//!   `chain` / `product` / `hw` / `hub` are shown only when present;
//! * a bare JSON array of devices is also accepted;
//! * `chain: "unknown"` is an ABSENT claim, not a failure — a display holds no
//!   witness chain of its own, and painting it as trouble is a lie about every
//!   display in the fleet.
//!
//! This is coarse presence and health — never secrets, never media.

use serde::{Deserialize, Serialize};

/// A device that omits `online` is NOT claimed present. The phone's decoder
/// and the Wall's Swift `Device` already defaulted to false ("absent is not a
/// presence claim — never rendered as online"); this normalizer said true, so
/// the two halves of the same app disagreed about the same silent field.
fn default_online() -> bool {
    false
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
    fn a_device_that_omits_the_new_fields_still_parses() {
        // Every device on firmware older than these fields, which is all of
        // them until the fleet takes an update. Absent must stay absent —
        // never "" and never a default that reads as an answer.
        let fleet =
            parse_fleet(r#"{"devices":[{"name":"Porch","product":"canary-wap"}]}"#).unwrap();
        assert_eq!(fleet.devices[0].hw, None);
        assert_eq!(fleet.devices[0].hub, None);
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
