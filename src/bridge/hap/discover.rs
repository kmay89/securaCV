//! Looking around the network, so the wizard can tell the user what is there
//! instead of asking them.
//!
//! "Do you have an Apple TV?" is a bad question. The user may not know
//! whether the one in the spare room counts, whether it is signed in, or
//! whether a HomePod mini is a hub (it is). We can just look: Apple hubs
//! advertise themselves over mDNS, and so does every HomeKit accessory
//! already in the house.
//!
//! # What this can and cannot tell you
//!
//! Seeing an Apple TV on the network proves it is **there**, not that it is
//! **signed into your Apple Account and enabled as a Home Hub** — that is a
//! setting inside the Home app that no amount of network traffic reveals. So
//! everything here is phrased as evidence, never as a verdict, and the wizard
//! says "found" rather than "ready". Overstating this would be the same sin
//! as an unbenchmarked performance claim.
//!
//! Discovery is also **best-effort by construction**: mDNS is multicast UDP
//! on a timer. A quiet network, a sleeping Apple TV, or a router that does
//! not forward multicast all produce an empty list, and an empty list must
//! never block setup — it downgrades the wizard to asking, which is where it
//! would have started anyway.

use std::collections::BTreeMap;
use std::time::{Duration, Instant};

/// The mDNS service Apple hubs advertise on.
pub const AIRPLAY_SERVICE: &str = "_airplay._tcp.local.";

/// Existing HomeKit accessories, so the wizard can say what is already paired
/// into this home.
pub const HAP_SERVICE: &str = "_hap._tcp.local.";

/// What kind of Apple device answered.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum HubKind {
    /// An Apple TV. Any model signed in can act as a Home Hub.
    AppleTv,
    /// A HomePod or HomePod mini — also a hub.
    HomePod,
    /// A Mac. Macs run AirPlay receivers but are **not** Home Hubs, so this
    /// is tracked separately rather than counted as one.
    Mac,
    /// Something else advertising AirPlay (a TV with AirPlay 2, a receiver).
    /// Not a hub.
    Other,
}

impl HubKind {
    /// Whether a device of this kind can act as a Home Hub.
    ///
    /// This is the distinction that matters: without a hub, HomeKit works
    /// only while the phone is on the same network, and automations do not
    /// run at all. A Mac advertising AirPlay looks identical to an Apple TV
    /// on a casual glance at mDNS, and telling a user their Mac is a hub
    /// would send them chasing a bug that isn't one.
    pub fn is_home_hub(self) -> bool {
        matches!(self, HubKind::AppleTv | HubKind::HomePod)
    }

    /// A human label.
    pub fn label(self) -> &'static str {
        match self {
            HubKind::AppleTv => "Apple TV",
            HubKind::HomePod => "HomePod",
            HubKind::Mac => "Mac",
            HubKind::Other => "AirPlay device",
        }
    }
}

/// Classify a device from its mDNS `model` TXT record.
///
/// Apple's model strings are stable and prefix-shaped (`AppleTV14,1`,
/// `AudioAccessory5,1`, `Macmini9,1`), which is what makes this a prefix
/// match rather than a table that would need updating for every new product.
pub fn classify(model: &str) -> HubKind {
    let m = model.trim();
    if m.starts_with("AppleTV") {
        HubKind::AppleTv
    } else if m.starts_with("AudioAccessory") {
        HubKind::HomePod
    } else if m.starts_with("Mac") || m.starts_with("iMac") || m.starts_with("Book") {
        HubKind::Mac
    } else {
        HubKind::Other
    }
}

/// One device found on the network.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Found {
    /// The advertised name, e.g. "Living Room".
    pub name: String,
    /// What it is.
    pub kind: HubKind,
}

/// Browse for Apple devices that could be Home Hubs.
///
/// Returns whatever answered within `window`, de-duplicated by name. Never
/// errors into the caller's face: a network that says nothing yields an empty
/// list, because "I could not look" and "there is nothing there" should lead
/// to the same next step — ask.
pub fn find_apple_devices(window: Duration) -> Vec<Found> {
    let Ok(daemon) = mdns_sd::ServiceDaemon::new() else {
        return Vec::new();
    };
    let Ok(rx) = daemon.browse(AIRPLAY_SERVICE) else {
        return Vec::new();
    };

    let mut seen: BTreeMap<String, HubKind> = BTreeMap::new();
    let deadline = Instant::now() + window;
    while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
        match rx.recv_timeout(remaining) {
            Ok(mdns_sd::ServiceEvent::ServiceResolved(info)) => {
                let name = friendly_name(info.get_fullname());
                let model = info
                    .get_property_val_str("model")
                    .unwrap_or_default()
                    .to_string();
                seen.insert(name, classify(&model));
            }
            Ok(_) => {}
            Err(_) => break,
        }
    }
    let _ = daemon.shutdown();

    seen.into_iter()
        .map(|(name, kind)| Found { name, kind })
        .collect()
}

/// Strip the service suffix off an mDNS fullname, and un-escape the `\ `
/// that mDNS uses for spaces — so "Living\032Room._airplay._tcp.local." reads
/// back as "Living Room".
pub fn friendly_name(fullname: &str) -> String {
    let instance = fullname
        .split_once("._")
        .map(|(head, _)| head)
        .unwrap_or(fullname);
    unescape_mdns(instance)
}

fn unescape_mdns(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        // `\DDD` is a decimal byte escape; `\x` is a literal x.
        let digits: String = chars.clone().take(3).collect();
        if digits.len() == 3 && digits.chars().all(|d| d.is_ascii_digit()) {
            if let Ok(byte) = digits.parse::<u8>() {
                out.push(char::from(byte));
                for _ in 0..3 {
                    chars.next();
                }
                continue;
            }
        }
        if let Some(next) = chars.next() {
            out.push(next);
        }
    }
    out
}

/// A one-line summary of what was found, for the wizard to print.
///
/// Deliberately phrased as evidence. "Found" is a fact about the network;
/// "ready" would be a claim about a setting we cannot see.
pub fn summarize(found: &[Found]) -> String {
    let hubs: Vec<&Found> = found.iter().filter(|f| f.kind.is_home_hub()).collect();
    if hubs.is_empty() {
        return "No Apple TV or HomePod answered on this network.".to_string();
    }
    let names: Vec<String> = hubs
        .iter()
        .map(|f| format!("{} ({})", f.name, f.kind.label()))
        .collect();
    format!("Found {}: {}", plural(hubs.len(), "hub"), names.join(", "))
}

fn plural(n: usize, word: &str) -> String {
    if n == 1 {
        format!("1 {word}")
    } else {
        format!("{n} {word}s")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn apple_model_strings_classify() {
        assert_eq!(classify("AppleTV14,1"), HubKind::AppleTv);
        assert_eq!(classify("AppleTV11,1"), HubKind::AppleTv);
        assert_eq!(classify("AudioAccessory5,1"), HubKind::HomePod);
        assert_eq!(classify("AudioAccessory1,1"), HubKind::HomePod);
        assert_eq!(classify("Macmini9,1"), HubKind::Mac);
        assert_eq!(classify("MacBookPro18,3"), HubKind::Mac);
        assert_eq!(classify("iMac21,1"), HubKind::Mac);
    }

    /// The distinction the whole feature leans on: a Mac advertising AirPlay
    /// is not a Home Hub, and saying it is would send a user chasing a
    /// non-bug when their automations don't run.
    #[test]
    fn only_apple_tv_and_homepod_count_as_hubs() {
        assert!(HubKind::AppleTv.is_home_hub());
        assert!(HubKind::HomePod.is_home_hub());
        assert!(!HubKind::Mac.is_home_hub());
        assert!(!HubKind::Other.is_home_hub());
    }

    #[test]
    fn unknown_models_are_other_not_a_guess() {
        assert_eq!(classify("Samsung-TV"), HubKind::Other);
        assert_eq!(classify(""), HubKind::Other);
        assert_eq!(classify("   "), HubKind::Other);
    }

    #[test]
    fn mdns_names_are_unescaped_for_humans() {
        assert_eq!(
            friendly_name("Living\\032Room._airplay._tcp.local."),
            "Living Room"
        );
        assert_eq!(friendly_name("Kitchen._airplay._tcp.local."), "Kitchen");
        // A name containing a dot survives, because we split on "._" not "."
        assert_eq!(
            friendly_name("Mr.\\032Bird._airplay._tcp.local."),
            "Mr. Bird"
        );
    }

    #[test]
    fn a_name_with_no_suffix_passes_through() {
        assert_eq!(friendly_name("Bare"), "Bare");
    }

    /// The summary must never promise more than the network showed.
    #[test]
    fn the_summary_reports_evidence_not_readiness() {
        let found = vec![
            Found {
                name: "Living Room".into(),
                kind: HubKind::AppleTv,
            },
            Found {
                name: "Studio".into(),
                kind: HubKind::Mac,
            },
        ];
        let s = summarize(&found);
        assert!(s.contains("Found 1 hub"), "got: {s}");
        assert!(s.contains("Living Room (Apple TV)"), "got: {s}");
        assert!(!s.contains("Studio"), "a Mac is not a hub: {s}");
        assert!(
            !s.to_lowercase().contains("ready"),
            "must not claim readiness"
        );
    }

    #[test]
    fn no_hubs_says_so_plainly() {
        assert!(summarize(&[]).starts_with("No Apple TV or HomePod"));
        let macs = vec![Found {
            name: "Studio".into(),
            kind: HubKind::Mac,
        }];
        assert!(summarize(&macs).starts_with("No Apple TV or HomePod"));
    }

    #[test]
    fn several_hubs_pluralize() {
        let found = vec![
            Found {
                name: "Living Room".into(),
                kind: HubKind::AppleTv,
            },
            Found {
                name: "Kitchen".into(),
                kind: HubKind::HomePod,
            },
        ];
        assert!(summarize(&found).contains("Found 2 hubs"));
    }

    /// A quiet network must yield an empty list quickly rather than hang —
    /// setup has to survive a router that drops multicast.
    #[test]
    fn discovery_gives_up_rather_than_hanging() {
        let start = Instant::now();
        let _ = find_apple_devices(Duration::from_millis(120));
        assert!(
            start.elapsed() < Duration::from_secs(5),
            "discovery must be bounded by its window"
        );
    }
}
