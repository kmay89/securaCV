//! The Home-app face of a fleet: HAP accessories, services and
//! characteristics built from the [`HomeSignal`] table.
//!
//! # One accessory per Canary, behind a bridge
//!
//! `witnessd` can witness for more than one Canary, so it presents itself as
//! a **bridge** (HAP category 2): accessory ID 1 is the bridge itself, and
//! every Canary is a bridged accessory from ID 2 up. That is the epic's
//! open decision #6, settled the way the Home app reads best — "Porch
//! Canary" is one tile you can put in a room, not five loose sensors.
//!
//! # Which signals become services, and which become status
//!
//! HAP already distinguishes these, and we follow it rather than inventing a
//! parallel scheme:
//!
//! - **Sensor signals** (motion, occupancy, contact, and the four
//!   class-scoped motion signals) each become their own *service* — a tile
//!   in the Home app with its own name and its own automation trigger.
//! - **Status signals** (tamper, active, low battery) are *optional
//!   characteristics* that HAP defines on sensor services, so they attach to
//!   every sensor service rather than becoming standalone tiles. This is why
//!   a tampered Canary shows a warning **on the sensor you already look at**
//!   instead of appearing as a mystery switch.
//!
//! # Instance IDs are stable, and that is load-bearing
//!
//! Controllers cache `(aid, iid)` pairs and re-subscribe to them across
//! restarts. So iids are allocated from the **full** [`HomeSignal::ALL`]
//! table, not from the enabled subset: turning a signal off leaves every
//! other signal's iid exactly where it was. Allocating from the enabled set
//! would renumber the accessory whenever a user changed their consent — the
//! Home app would silently start reading the wrong sensor.

use super::super::homekit::{HomeSignal, Publication, SignalSet};

/// HAP service type UUIDs (the short form Apple defines for standard types).
mod svc {
    pub const ACCESSORY_INFORMATION: &str = "3E";
    pub const PROTOCOL_INFORMATION: &str = "A2";
    pub const MOTION_SENSOR: &str = "85";
    pub const OCCUPANCY_SENSOR: &str = "86";
    pub const CONTACT_SENSOR: &str = "80";
}

/// HAP characteristic type UUIDs.
mod chr {
    pub const IDENTIFY: &str = "14";
    pub const MANUFACTURER: &str = "20";
    pub const MODEL: &str = "21";
    pub const NAME: &str = "23";
    pub const SERIAL_NUMBER: &str = "30";
    pub const FIRMWARE_REVISION: &str = "52";
    pub const VERSION: &str = "37";

    pub const MOTION_DETECTED: &str = "22";
    pub const OCCUPANCY_DETECTED: &str = "71";
    pub const CONTACT_SENSOR_STATE: &str = "6A";
    pub const STATUS_TAMPERED: &str = "7A";
    pub const STATUS_ACTIVE: &str = "75";
    pub const STATUS_LOW_BATTERY: &str = "79";
}

/// The accessory ID of the bridge itself. Bridged Canaries start at 2.
pub const BRIDGE_AID: u64 = 1;

/// Where a signal's characteristic block starts inside an accessory.
///
/// Chosen to leave iids 1..9 for the Accessory Information service, and to
/// give every signal a 10-wide block so status characteristics have fixed
/// offsets inside it.
const SIGNAL_IID_BASE: u64 = 10;
const SIGNAL_IID_STRIDE: u64 = 10;

/// Offsets within a sensor service's iid block. Offset 0 is the service
/// itself, so the characteristics start at 1.
const OFF_PRIMARY: u64 = 1;
const OFF_NAME: u64 = 2;
const OFF_TAMPERED: u64 = 3;
const OFF_ACTIVE: u64 = 4;
const OFF_LOW_BATTERY: u64 = 5;

/// How a signal is expressed in HAP.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Expression {
    /// Gets its own service — a tile in the Home app.
    Sensor,
    /// Rides along as an optional characteristic on every sensor service.
    Status,
}

/// Whether this signal becomes a service or a status characteristic.
pub fn expression(sig: HomeSignal) -> Expression {
    match sig {
        HomeSignal::Tamper | HomeSignal::Active | HomeSignal::LowBattery => Expression::Status,
        _ => Expression::Sensor,
    }
}

/// The service type a sensor signal projects as.
fn service_type(sig: HomeSignal) -> Option<&'static str> {
    match sig {
        HomeSignal::Motion
        | HomeSignal::MotionPerson
        | HomeSignal::MotionVehicle
        | HomeSignal::MotionAnimal
        | HomeSignal::MotionPackage => Some(svc::MOTION_SENSOR),
        HomeSignal::Occupancy => Some(svc::OCCUPANCY_SENSOR),
        HomeSignal::Contact => Some(svc::CONTACT_SENSOR),
        HomeSignal::Tamper | HomeSignal::Active | HomeSignal::LowBattery => None,
    }
}

/// The characteristic type a signal's value is written to.
fn characteristic_type(sig: HomeSignal) -> &'static str {
    match sig {
        HomeSignal::Motion
        | HomeSignal::MotionPerson
        | HomeSignal::MotionVehicle
        | HomeSignal::MotionAnimal
        | HomeSignal::MotionPackage => chr::MOTION_DETECTED,
        HomeSignal::Occupancy => chr::OCCUPANCY_DETECTED,
        HomeSignal::Contact => chr::CONTACT_SENSOR_STATE,
        HomeSignal::Tamper => chr::STATUS_TAMPERED,
        HomeSignal::Active => chr::STATUS_ACTIVE,
        HomeSignal::LowBattery => chr::STATUS_LOW_BATTERY,
    }
}

/// The characteristic's HAP value format. Motion and active are booleans;
/// the rest are small enums HAP models as `uint8`.
fn value_format(sig: HomeSignal) -> &'static str {
    match sig {
        HomeSignal::Motion
        | HomeSignal::MotionPerson
        | HomeSignal::MotionVehicle
        | HomeSignal::MotionAnimal
        | HomeSignal::MotionPackage
        | HomeSignal::Active => "bool",
        _ => "uint8",
    }
}

/// The human-readable name of a signal's service, as it appears in the Home
/// app under the accessory's name.
fn service_name(sig: HomeSignal) -> &'static str {
    match sig {
        HomeSignal::Motion => "Motion",
        HomeSignal::Occupancy => "Occupancy",
        HomeSignal::Contact => "Contact",
        HomeSignal::MotionPerson => "Person",
        HomeSignal::MotionVehicle => "Vehicle",
        HomeSignal::MotionAnimal => "Animal",
        HomeSignal::MotionPackage => "Package",
        HomeSignal::Tamper => "Tamper",
        HomeSignal::Active => "Active",
        HomeSignal::LowBattery => "Battery",
    }
}

/// The index of a signal in [`HomeSignal::ALL`] — the basis of its stable iid
/// block.
fn signal_index(sig: HomeSignal) -> u64 {
    HomeSignal::ALL.iter().position(|s| *s == sig).unwrap_or(0) as u64
}

/// The first iid of a signal's block. Stable for the life of the accessory,
/// independent of which signals are enabled.
fn signal_block(sig: HomeSignal) -> u64 {
    SIGNAL_IID_BASE + signal_index(sig) * SIGNAL_IID_STRIDE
}

/// The iid of the characteristic carrying `value_sig`, on the service of
/// `host_sig`.
///
/// For a sensor signal the two are the same and it is the service's primary
/// characteristic. For a status signal, `host_sig` is the sensor service it
/// is attached to, which is why the same status signal has a different iid on
/// each service — iids are unique per accessory, not per characteristic type.
fn characteristic_iid(host_sig: HomeSignal, value_sig: HomeSignal) -> u64 {
    let base = signal_block(host_sig);
    match value_sig {
        s if s == host_sig => base + OFF_PRIMARY,
        HomeSignal::Tamper => base + OFF_TAMPERED,
        HomeSignal::Active => base + OFF_ACTIVE,
        HomeSignal::LowBattery => base + OFF_LOW_BATTERY,
        _ => base + OFF_PRIMARY,
    }
}

/// A characteristic's current value, rendered as JSON.
fn value_json(sig: HomeSignal, asserted: bool) -> String {
    match value_format(sig) {
        "bool" => {
            // HAP booleans go on the wire as true/false.
            if asserted {
                "true".into()
            } else {
                "false".into()
            }
        }
        _ => {
            // Contact is the one signal whose HAP enum is not "1 means the
            // interesting thing": ContactSensorState is 0 = contact detected
            // (closed), 1 = not detected (open). Our Contact signal asserts
            // when the contact is OPEN, so it maps straight to 1/0 — but the
            // reasoning is written down because the polarity is easy to
            // invert by accident and the failure is silent.
            if asserted {
                "1".into()
            } else {
                "0".into()
            }
        }
    }
}

/// One Canary, as the Home app will see it.
#[derive(Clone, Debug)]
pub struct CanaryAccessory {
    /// HAP accessory ID. Unique within the bridge, stable across restarts.
    pub aid: u64,
    /// The name shown in the Home app, e.g. "Porch Canary".
    pub name: String,
    /// Stable per-device serial, shown in the accessory's detail view.
    pub serial: String,
    /// Which signals this Canary publishes.
    pub enabled: SignalSet,
    /// The most recent publication from the pacer.
    pub last: Publication,
}

impl CanaryAccessory {
    /// A Canary with the default (dumb-PIR bar) signal set and everything
    /// clear.
    pub fn new(aid: u64, name: impl Into<String>, serial: impl Into<String>) -> Self {
        CanaryAccessory {
            aid,
            name: name.into(),
            serial: serial.into(),
            enabled: SignalSet::default_enabled(),
            last: Publication {
                seq: 0,
                asserted: SignalSet::new(),
            },
        }
    }

    /// The sensor signals that get their own service, in table order.
    fn sensor_signals(&self) -> impl Iterator<Item = HomeSignal> + '_ {
        HomeSignal::ALL
            .into_iter()
            .filter(move |s| self.enabled.contains(*s) && expression(*s) == Expression::Sensor)
    }

    /// The status signals attached to every sensor service, in table order.
    fn status_signals(&self) -> impl Iterator<Item = HomeSignal> + '_ {
        HomeSignal::ALL
            .into_iter()
            .filter(move |s| self.enabled.contains(*s) && expression(*s) == Expression::Status)
    }

    /// Every `(iid, signal)` this accessory exposes — the lookup a
    /// `/characteristics` read or an event notification resolves against.
    pub fn characteristics(&self) -> Vec<(u64, HomeSignal)> {
        let mut out = Vec::new();
        for host in self.sensor_signals() {
            out.push((characteristic_iid(host, host), host));
            for status in self.status_signals() {
                out.push((characteristic_iid(host, status), status));
            }
        }
        out
    }

    /// The current value of the characteristic at `iid`, as JSON, if this
    /// accessory has one there.
    pub fn value_at(&self, iid: u64) -> Option<String> {
        self.characteristics()
            .into_iter()
            .find(|(i, _)| *i == iid)
            .map(|(_, sig)| value_json(sig, self.last.asserted.contains(sig)))
    }

    /// This accessory's entry in the `/accessories` document.
    fn to_json(&self) -> String {
        let mut services = vec![accessory_information_json(
            1,
            &self.name,
            &self.serial,
            "Canary",
        )];

        for host in self.sensor_signals() {
            let base = signal_block(host);
            let ty = service_type(host).unwrap_or(svc::MOTION_SENSOR);
            let mut chars = vec![
                characteristic_json(
                    characteristic_iid(host, host),
                    characteristic_type(host),
                    value_format(host),
                    &value_json(host, self.last.asserted.contains(host)),
                ),
                // A per-service Name is what makes the Home app show "Porch
                // Canary — Person" rather than four identical motion tiles.
                string_characteristic_json(base + OFF_NAME, chr::NAME, service_name(host)),
            ];
            for status in self.status_signals() {
                chars.push(characteristic_json(
                    characteristic_iid(host, status),
                    characteristic_type(status),
                    value_format(status),
                    &value_json(status, self.last.asserted.contains(status)),
                ));
            }
            services.push(format!(
                r#"{{"iid":{base},"type":"{ty}","characteristics":[{}]}}"#,
                chars.join(",")
            ));
        }

        format!(
            r#"{{"aid":{},"services":[{}]}}"#,
            self.aid,
            services.join(",")
        )
    }
}

fn escape_json(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

fn characteristic_json(iid: u64, ty: &str, format: &str, value: &str) -> String {
    format!(
        r#"{{"iid":{iid},"type":"{ty}","format":"{format}","perms":["pr","ev"],"value":{value}}}"#
    )
}

fn string_characteristic_json(iid: u64, ty: &str, value: &str) -> String {
    format!(
        r#"{{"iid":{iid},"type":"{ty}","format":"string","perms":["pr"],"value":"{}"}}"#,
        escape_json(value)
    )
}

/// The Accessory Information service every HAP accessory must carry.
fn accessory_information_json(iid: u64, name: &str, serial: &str, model: &str) -> String {
    let chars = [
        format!(
            r#"{{"iid":{},"type":"{}","format":"bool","perms":["pw"]}}"#,
            iid + 1,
            chr::IDENTIFY
        ),
        string_characteristic_json(iid + 2, chr::MANUFACTURER, "Errer Labs"),
        string_characteristic_json(iid + 3, chr::MODEL, model),
        string_characteristic_json(iid + 4, chr::NAME, name),
        string_characteristic_json(iid + 5, chr::SERIAL_NUMBER, serial),
        string_characteristic_json(iid + 6, chr::FIRMWARE_REVISION, env!("CARGO_PKG_VERSION")),
    ];
    format!(
        r#"{{"iid":{iid},"type":"{}","characteristics":[{}]}}"#,
        svc::ACCESSORY_INFORMATION,
        chars.join(",")
    )
}

/// The bridge accessory itself — aid 1, no sensors, just identity.
fn bridge_json(name: &str, serial: &str) -> String {
    let info = accessory_information_json(1, name, serial, "SecuraCV Bridge");
    let protocol = format!(
        r#"{{"iid":20,"type":"{}","characteristics":[{{"iid":21,"type":"{}","format":"string","perms":["pr"],"value":"1.1.0"}}]}}"#,
        svc::PROTOCOL_INFORMATION,
        chr::VERSION
    );
    format!(r#"{{"aid":{BRIDGE_AID},"services":[{info},{protocol}]}}"#)
}

/// The whole fleet, as the `/accessories` document.
pub fn accessories_json(
    bridge_name: &str,
    bridge_serial: &str,
    fleet: &[CanaryAccessory],
) -> String {
    let mut all = vec![bridge_json(bridge_name, bridge_serial)];
    all.extend(fleet.iter().map(CanaryAccessory::to_json));
    format!(r#"{{"accessories":[{}]}}"#, all.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn canary() -> CanaryAccessory {
        CanaryAccessory::new(2, "Porch Canary", "CNRY-0001")
    }

    /// The property the whole iid scheme exists for: turning a signal off
    /// must not move any other signal's iid. A controller that cached
    /// `(aid, iid)` keeps reading the sensor it thinks it is reading.
    #[test]
    fn iids_are_stable_when_consent_changes() {
        let mut a = canary();
        let before: Vec<_> = a.characteristics();
        let motion_iid = before
            .iter()
            .find(|(_, s)| *s == HomeSignal::Motion)
            .map(|(i, _)| *i)
            .expect("motion is enabled by default");

        // Enable a class-scoped signal, which sits *after* motion in the table.
        a.enabled.insert(HomeSignal::MotionPerson);
        let after = a.characteristics();
        let motion_iid_after = after
            .iter()
            .find(|(_, s)| *s == HomeSignal::Motion)
            .map(|(i, _)| *i)
            .expect("motion still enabled");
        assert_eq!(motion_iid, motion_iid_after);

        // And removing one does not shift the others either.
        a.enabled.remove(HomeSignal::Occupancy);
        let after_removal = a.characteristics();
        assert_eq!(
            after_removal
                .iter()
                .find(|(_, s)| *s == HomeSignal::Motion)
                .map(|(i, _)| *i),
            Some(motion_iid)
        );
    }

    /// Every iid within an accessory must be unique, including the status
    /// characteristics repeated across services.
    #[test]
    fn iids_are_unique_within_an_accessory() {
        let mut a = canary();
        for sig in HomeSignal::ALL {
            a.enabled.insert(sig);
        }
        let chars = a.characteristics();
        let mut seen = std::collections::HashSet::new();
        for (iid, _) in &chars {
            assert!(seen.insert(*iid), "duplicate iid {iid}");
        }
        assert!(chars.len() > 10, "expected a populated accessory");
    }

    #[test]
    fn status_signals_do_not_become_services() {
        assert_eq!(expression(HomeSignal::Tamper), Expression::Status);
        assert_eq!(expression(HomeSignal::Active), Expression::Status);
        assert_eq!(expression(HomeSignal::LowBattery), Expression::Status);
        assert_eq!(expression(HomeSignal::Motion), Expression::Sensor);
        assert_eq!(service_type(HomeSignal::Tamper), None);
    }

    /// Tamper must be visible on the sensor the user actually looks at.
    #[test]
    fn tamper_rides_on_every_sensor_service() {
        let a = canary();
        let sensors = a.sensor_signals().count();
        let tamper_chars = a
            .characteristics()
            .into_iter()
            .filter(|(_, s)| *s == HomeSignal::Tamper)
            .count();
        assert_eq!(tamper_chars, sensors);
        assert!(sensors >= 3, "default set has motion, occupancy, contact");
    }

    #[test]
    fn class_scoped_signals_are_absent_until_enabled() {
        let mut a = canary();
        assert!(
            !a.characteristics()
                .iter()
                .any(|(_, s)| *s == HomeSignal::MotionPerson),
            "class-scoped signals are off at the dumb-PIR bar"
        );
        a.enabled.insert(HomeSignal::MotionPerson);
        assert!(a
            .characteristics()
            .iter()
            .any(|(_, s)| *s == HomeSignal::MotionPerson));
    }

    /// Contact's HAP enum is inverted relative to the others; assert the
    /// polarity so a future refactor cannot silently flip every door.
    #[test]
    fn contact_polarity_is_open_equals_one() {
        assert_eq!(value_json(HomeSignal::Contact, true), "1");
        assert_eq!(value_json(HomeSignal::Contact, false), "0");
        assert_eq!(value_json(HomeSignal::Motion, true), "true");
        assert_eq!(value_json(HomeSignal::Motion, false), "false");
    }

    #[test]
    fn accessories_document_has_bridge_first() {
        let doc = accessories_json("SecuraCV", "BRIDGE-1", &[canary()]);
        assert!(doc.starts_with(r#"{"accessories":[{"aid":1,"#));
        assert!(doc.contains(r#""aid":2"#));
        assert!(doc.contains("Porch Canary"));
        // The bridge advertises the HAP protocol version it speaks.
        assert!(doc.contains(r#""value":"1.1.0""#));
    }

    #[test]
    fn value_at_resolves_live_state() {
        let mut a = canary();
        let motion_iid = a
            .characteristics()
            .into_iter()
            .find(|(_, s)| *s == HomeSignal::Motion)
            .map(|(i, _)| i)
            .expect("motion enabled");
        assert_eq!(a.value_at(motion_iid).as_deref(), Some("false"));

        let mut asserted = SignalSet::new();
        asserted.insert(HomeSignal::Motion);
        a.last = Publication { seq: 1, asserted };
        assert_eq!(a.value_at(motion_iid).as_deref(), Some("true"));
        assert_eq!(a.value_at(9_999), None);
    }

    #[test]
    fn names_with_quotes_do_not_break_the_document() {
        let a = CanaryAccessory::new(2, r#"The "Back" Door"#, "CNRY-2");
        let doc = accessories_json("SecuraCV", "B", &[a]);
        assert!(doc.contains(r#"The \"Back\" Door"#));
    }
}
