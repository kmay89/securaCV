//! The alert relay's pure core (docs/design/alert_relay.md, the flagship lane).
//!
//! Everything here is decision, not delivery: map one MQTT `(topic, payload)`
//! to at most one metadata-only [`Poke`], and rate-limit per (class, device)
//! so one noisy reading never pages the owner. No socket, no clock — callers
//! pass monotonic seconds — so the whole privacy posture is unit-tested.
//!
//! The three rules from the design doc, made structural:
//! - **Metadata only.** A poke carries a severity class, a short owner-facing
//!   sentence, and (optionally) a link that points home. It has **no
//!   timestamp field** — event times are bucketed on purpose (Invariant III),
//!   and stamping "now" on a poke would rebuild the timing oracle the
//!   bucketing removed.
//! - **No stable device fingerprint in the cloud path.** The relay never
//!   subscribes `securacv/+/health` (carries `public_key`) or
//!   `securacv/+/chain` (carries `fp`); [`SUBSCRIBE_TOPICS`] is the whole
//!   list, and a test asserts the forbidden topics route to nothing.
//! - **The link points home.** The optional deep link is the owner's local
//!   UI, configured by the owner; the relay never invents a URL.
//!
//! The severity vocabulary is the four coarse classes the iPhone lane already
//! ships (`ios/Shared/WakePayload.swift` decodes the flat `{"sev": ...}`
//! envelope): `tamper`, `integrity`, `offline`, `pattern`. Same words here so
//! a future self-hosted push lane speaks one dialect.

use std::collections::HashMap;

/// The four coarse severity classes — the whole vocabulary that may leave
/// the LAN. Matches the shipped iOS wake vocabulary; do not widen casually.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PokeClass {
    /// A witness reports interference with itself. Top of the ladder.
    Tamper,
    /// The sealed log failed verification.
    Integrity,
    /// A device (or the bridge) went dark.
    Offline,
    /// A life-safety or anomaly pattern was heard/seen (smoke, CO).
    Pattern,
}

impl PokeClass {
    /// The wire word (the `sev` field iOS already decodes).
    pub fn sev(self) -> &'static str {
        match self {
            PokeClass::Tamper => "tamper",
            PokeClass::Integrity => "integrity",
            PokeClass::Offline => "offline",
            PokeClass::Pattern => "pattern",
        }
    }

    /// ntfy priority header value (1 low .. 5 urgent). Life-safety patterns
    /// and tamper page loudly; offline is a nudge.
    pub fn ntfy_priority(self) -> u8 {
        match self {
            PokeClass::Pattern | PokeClass::Tamper => 5,
            PokeClass::Integrity => 4,
            PokeClass::Offline => 3,
        }
    }

    /// Default minimum gap between pokes of this class for one device, in
    /// seconds. The design doc's rule: push sinks fire on threshold
    /// crossings with hysteresis. These are deliberately coarse; the config
    /// can widen but not narrow below 60 s.
    pub fn default_min_gap_secs(self) -> u64 {
        match self {
            PokeClass::Pattern => 600, // a sounding alarm re-pokes at most 10-minutely
            PokeClass::Tamper => 900,  // 15 min
            PokeClass::Offline => 1800, // 30 min — WiFi blips are weather
            PokeClass::Integrity => 3600, // 1 h — one page per broken chain is plenty
        }
    }
}

/// One outbound poke. No timestamp, no event content, no fingerprint —
/// the fields ARE the privacy contract.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Poke {
    pub class: PokeClass,
    /// Short title, fixed per lane — never composed from event content.
    pub title: &'static str,
    /// One coarse sentence. May name the device id from the topic (the
    /// owner-visible name half of the topic path, not a key or fingerprint).
    pub body: String,
    /// Which device this concerns, for debouncing. Empty for hub-level lanes.
    pub device: String,
}

/// The complete subscribe list. `health` and `chain` are absent on purpose —
/// they carry stable fingerprints (public_key / fp) that must never enter
/// the cloud path. Widening this list is a privacy decision, not a tweak.
pub const SUBSCRIBE_TOPICS: &[&str] = &[
    "securacv/+/tamper",
    "securacv/+/status",
    "securacv/+/availability",
    "securacv/+/sensing",
    "securacv/fleet/escalation",
    "witness/chain_problem",
];

fn device_from_topic(topic: &str) -> Option<&str> {
    let mut parts = topic.split('/');
    if parts.next()? != "securacv" {
        return None;
    }
    let device = parts.next()?;
    if device == "fleet" {
        return None;
    }
    Some(device)
}

fn json_field<'a>(value: &'a serde_json::Value, key: &str) -> Option<&'a str> {
    value.get(key).and_then(|v| v.as_str())
}

/// Map one message to at most one poke. Pure; the debouncer decides delivery.
pub fn evaluate(topic: &str, payload: &[u8]) -> Option<Poke> {
    let raw = std::str::from_utf8(payload).ok()?;

    if topic == "witness/chain_problem" {
        if raw.trim() != "ON" {
            return None;
        }
        return Some(Poke {
            class: PokeClass::Integrity,
            title: "Chain verification failed",
            body: "The witness log did not verify. Check the hub.".to_string(),
            device: String::new(),
        });
    }

    if topic == "securacv/fleet/escalation" {
        // Firmware publishes this only when a Tier-1 condition ran unacked
        // past its deadline — pre-debounced at the source, built for
        // automation consumers. Relay it as tamper-grade.
        return Some(Poke {
            class: PokeClass::Tamper,
            title: "Unacknowledged fleet escalation",
            body: "A Canary escalated and nobody acknowledged it.".to_string(),
            device: String::new(),
        });
    }

    let device = device_from_topic(topic)?.to_string();
    let suffix = topic.rsplit('/').next()?;

    match suffix {
        "tamper" => {
            let value: serde_json::Value = serde_json::from_str(raw).ok()?;
            let state_on = json_field(&value, "state").map(str::trim) == Some("on");
            if !state_on {
                return None;
            }
            Some(Poke {
                class: PokeClass::Tamper,
                title: "Tamper reported",
                body: format!("Canary {device} reports interference. Check it now."),
                device,
            })
        }
        "status" => {
            // The vision/sense family's LWT: JSON with status:"offline".
            let value: serde_json::Value = serde_json::from_str(raw).ok()?;
            if json_field(&value, "status") != Some("offline") {
                return None;
            }
            Some(Poke {
                class: PokeClass::Offline,
                title: "Canary went dark",
                body: format!("Canary {device} stopped answering."),
                device,
            })
        }
        "availability" => {
            // The canary base family's LWT: the bare string "offline".
            if raw.trim() != "offline" {
                return None;
            }
            Some(Poke {
                class: PokeClass::Offline,
                title: "Canary went dark",
                body: format!("Canary {device} stopped answering."),
                device,
            })
        }
        "sensing" => {
            // Gate on the enum field that means "now" — NEVER the cumulative
            // t3_detected/t4_detected counters, which stay nonzero forever
            // and would re-poke on every 60 s heartbeat.
            let value: serde_json::Value = serde_json::from_str(raw).ok()?;
            let (title, kind) = match json_field(&value, "acoustic_event") {
                Some("smoke_alarm_t3") => ("Smoke alarm heard", "a smoke alarm"),
                Some("co_alarm_t4") => ("CO alarm heard", "a carbon monoxide alarm"),
                _ => return None,
            };
            Some(Poke {
                class: PokeClass::Pattern,
                title,
                body: format!("Canary {device} heard {kind} sounding."),
                device,
            })
        }
        _ => None,
    }
}

/// Per-(class, device) rate limiter. Monotonic seconds come from the caller,
/// so the hysteresis rule is testable without a clock.
#[derive(Default)]
pub struct Debouncer {
    last_sent: HashMap<(PokeClass, String), u64>,
    /// Owner-configured minimum gap per class; missing entries use the
    /// class default. Values below 60 are clamped up — a relay that can
    /// fire every second is a pager, not a witness.
    pub min_gap_secs: HashMap<PokeClass, u64>,
}

impl Debouncer {
    /// Should this poke go out now? Records the send when it says yes.
    /// Is this poke's lane outside its gap? Pure read — a failed delivery
    /// must not consume the send slot, so callers check here and call
    /// [`Debouncer::record`] only after the poke actually went out.
    pub fn ready(&self, poke: &Poke, now_secs: u64) -> bool {
        let gap = self
            .min_gap_secs
            .get(&poke.class)
            .copied()
            .unwrap_or_else(|| poke.class.default_min_gap_secs())
            .max(60);
        match self.last_sent.get(&(poke.class, poke.device.clone())) {
            Some(&last) => now_secs.saturating_sub(last) >= gap,
            None => true,
        }
    }

    /// Mark this poke's lane as sent now. Call only after successful delivery.
    pub fn record(&mut self, poke: &Poke, now_secs: u64) {
        self.last_sent
            .insert((poke.class, poke.device.clone()), now_secs);
    }

    /// Check-and-record in one step, for callers whose delivery cannot fail.
    pub fn allow(&mut self, poke: &Poke, now_secs: u64) -> bool {
        let ok = self.ready(poke, now_secs);
        if ok {
            self.record(poke, now_secs);
        }
        ok
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fingerprint_bearing_topics_are_never_subscribed_or_routed() {
        // The rule is structural: health carries public_key, chain carries
        // fp. Neither may enter the cloud path, so neither is in the
        // subscribe list — and even if a message arrived, it routes nowhere.
        for topic in SUBSCRIBE_TOPICS {
            assert!(
                !topic.contains("health") && !topic.contains("chain")
                    || *topic == "witness/chain_problem",
                "subscribe list must not carry fingerprint topics"
            );
        }
        assert!(evaluate("securacv/canary-1/health", br#"{"public_key":"aa"}"#).is_none());
        assert!(evaluate("securacv/canary-1/chain", br#"{"fp":"aa"}"#).is_none());
    }

    #[test]
    fn a_poke_carries_no_timestamp_and_no_content() {
        let poke = evaluate(
            "securacv/porch/tamper",
            br#"{"state":"on","confidence":0.93,"kind":"enclosure_tamper"}"#,
        )
        .expect("tamper poke");
        assert_eq!(poke.class, PokeClass::Tamper);
        // The body is a fixed sentence plus the topic's device word — the
        // payload's kind/confidence never reach it.
        assert!(!poke.body.contains("enclosure"));
        assert!(!poke.body.contains("0.93"));
    }

    #[test]
    fn cumulative_counters_never_poke_after_the_alarm_clears() {
        // The 60 s heartbeat after a past detection: counters nonzero,
        // event back to "none". Silence — the exact false positive that
        // would burn ntfy's free tier in an afternoon.
        assert!(evaluate(
            "securacv/hall/sensing",
            br#"{"acoustic_event":"none","t3_detected":5,"t4_detected":2}"#,
        )
        .is_none());
        let poke = evaluate(
            "securacv/hall/sensing",
            br#"{"acoustic_event":"smoke_alarm_t3","t3_detected":1}"#,
        )
        .expect("smoke poke");
        assert_eq!(poke.class, PokeClass::Pattern);
        assert_eq!(poke.class.ntfy_priority(), 5);
    }

    #[test]
    fn offline_is_the_lwt_shape_not_any_status_beat() {
        assert!(evaluate(
            "securacv/porch/status",
            br#"{"device_id":"porch","status":"online","rssi":-60}"#,
        )
        .is_none());
        let poke = evaluate(
            "securacv/porch/status",
            br#"{"device_id":"porch","device_type":"wap","status":"offline","ts_ms":0}"#,
        )
        .expect("offline poke");
        assert_eq!(poke.class, PokeClass::Offline);
    }

    #[test]
    fn chain_problem_and_escalation_route_at_hub_level() {
        assert_eq!(
            evaluate("witness/chain_problem", b"ON")
                .expect("integrity")
                .class,
            PokeClass::Integrity
        );
        assert!(evaluate("witness/chain_problem", b"OFF").is_none());
        assert_eq!(
            evaluate("securacv/fleet/escalation", b"{}")
                .expect("escalation")
                .class,
            PokeClass::Tamper
        );
    }

    #[test]
    fn both_lwt_dialects_poke_offline() {
        // vision/sense: JSON status. canary base: bare "offline" on
        // availability. Both families must reach the owner.
        let json = evaluate(
            "securacv/porch/status",
            br#"{"device_id":"porch","status":"offline","ts_ms":0}"#,
        )
        .expect("json lwt");
        assert_eq!(json.class, PokeClass::Offline);
        let bare = evaluate("securacv/porch/availability", b"offline").expect("bare lwt");
        assert_eq!(bare.class, PokeClass::Offline);
        assert!(evaluate("securacv/porch/availability", b"online").is_none());
    }

    #[test]
    fn a_failed_delivery_does_not_consume_the_send_slot() {
        let mut d = Debouncer::default();
        let poke = evaluate("securacv/porch/availability", b"offline").unwrap();
        assert!(d.ready(&poke, 1000));
        // Delivery failed: nothing recorded, the lane stays ready.
        assert!(d.ready(&poke, 1001), "no record until the poke went out");
        d.record(&poke, 1001);
        assert!(!d.ready(&poke, 1002), "recorded send starts the gap");
    }

    #[test]
    fn debounce_holds_the_gap_per_device_and_class() {
        let mut d = Debouncer::default();
        let poke = evaluate(
            "securacv/porch/tamper",
            br#"{"state":"on","kind":"enclosure_tamper"}"#,
        )
        .unwrap();
        assert!(d.allow(&poke, 1000));
        assert!(!d.allow(&poke, 1000 + 60), "inside the tamper gap: held");
        assert!(d.allow(&poke, 1000 + 900), "gap elapsed: fires");
        // A different device is its own lane.
        let other = Poke {
            device: "garage".into(),
            ..poke.clone()
        };
        assert!(d.allow(&other, 1001));
        // Config can widen but never narrow below the floor.
        let mut tight = Debouncer::default();
        tight.min_gap_secs.insert(PokeClass::Tamper, 1);
        assert!(tight.allow(&poke, 2000));
        assert!(!tight.allow(&poke, 2030), "sub-minute gaps clamp to 60 s");
        assert!(tight.allow(&poke, 2061));
    }
}
