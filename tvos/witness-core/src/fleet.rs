//! The `GET /api/fleet` contract, parsed once in Rust so the TV, the emulator,
//! and any future client agree on what a fleet *is*.
//!
//! The contract (and the browser rules that decide where it can work) is
//! `tvos/discovery/DISCOVERY.md`. Two rules from it are load-bearing and
//! enforced by the tests below:
//!
//! * only `devices[].name` is required; `online` defaults to `true`, and
//!   `chain` / `product` are shown only when present;
//! * a bare JSON array of devices is also accepted.
//!
//! This is coarse presence and health — never secrets, never media.

use serde::{Deserialize, Serialize};

fn default_online() -> bool {
    true
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

    /// True when any device reports a chain state that is not `ok`. The Wall
    /// surfaces this loudly — a fleet is not "fine" because most of it is.
    pub fn has_chain_trouble(&self) -> bool {
        self.devices
            .iter()
            .any(|d| matches!(d.chain.as_deref(), Some(c) if c != "ok"))
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
        { "name": "Front Door", "online": true,  "chain": "ok", "product": "canary-wap" },
        { "name": "Studio",     "online": true,  "chain": "ok", "product": "canary" },
        { "name": "Driveway",   "online": false, "chain": "ok", "product": "canary-vision" }
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
    }

    #[test]
    fn only_name_is_required_and_online_defaults_true() {
        let fleet = parse_fleet(r#"{"devices":[{"name":"Porch"}]}"#).unwrap();
        assert!(fleet.devices[0].online, "online must default to true");
        assert_eq!(fleet.devices[0].chain, None);
        assert_eq!(fleet.summary(), "1 Canary, all online");
    }

    #[test]
    fn a_bare_array_of_devices_is_accepted() {
        let fleet = parse_fleet(r#"[{"name":"Front Door"},{"name":"Studio"}]"#).unwrap();
        assert_eq!(fleet.devices.len(), 2);
        assert_eq!(fleet.summary(), "2 Canaries, all online");
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
