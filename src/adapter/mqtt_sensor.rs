//! Generic, config-driven MQTT sensor adapter.
//!
//! This is the "bring any cheap sensor" adapter: it maps incoming MQTT messages to coarse
//! [`Claim`]s using an operator-declared routing table (`topic -> (ClaimKind, zone, floor)`).
//! It covers acoustic/impulse sensors, PIR/contact switches, webhook→MQTT bridges, BLE-beacon
//! gateways — anything that can publish a simple message — **without writing any code**.
//!
//! Like [`FrigateAdapter`](crate::adapter::frigate::FrigateAdapter), it owns the receiving end of
//! a channel; a feeder pushes `(topic, payload)` pairs into the returned [`Sender`].
//!
//! Payloads may be:
//! - a JSON object, optionally `{"confidence": 0.0..=1.0, "zone": "label", "state": "on"|"off"}`,
//! - a bare string like `"ON"`/`"OFF"`/`"1"`/`"0"` (treated as a boolean state), or
//! - anything else (treated as a present/triggered signal with confidence 1.0).
//!
//! The generic adapter declares the full coarse claim vocabulary; the kernel still enforces the
//! contract (zone regex, confidence bounds, time coarsening) on every claim.

use std::sync::mpsc::{channel, Receiver, Sender};

use anyhow::Result;
use serde::Deserialize;

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::{LockTolerant, SensorAdapter};
use crate::{Attestation, EventType};

static MQTT_SENSOR_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "mqtt_sensor_adapter",
    allowed_claim_kinds: &[
        ClaimKind::LargeObjectBoundaryCrossing,
        ClaimKind::SmallObjectBoundaryCrossing,
        ClaimKind::AcousticImpulseInZone,
        ClaimKind::PresenceInRestrictedZone,
        ClaimKind::VehiclePresenceAfterHours,
        ClaimKind::ContactStateChange,
        ClaimKind::ObjectRemovedFromZone,
        ClaimKind::TamperDetected,
        ClaimKind::VehicleArrivalDeparture,
    ],
    allowed_event_types: &[
        EventType::BoundaryCrossingObjectLarge,
        EventType::BoundaryCrossingObjectSmall,
        EventType::AcousticImpulseInZone,
        EventType::PresenceInRestrictedZone,
        EventType::VehiclePresenceAfterHours,
        EventType::ContactStateChange,
        EventType::ObjectRemovedFromZone,
        EventType::TamperDetected,
        EventType::VehicleArrivalDeparture,
    ],
    requested_capabilities: &[],
};

/// One routing rule: messages on `topic` become a `kind` claim in `zone_label`.
#[derive(Clone, Debug)]
pub struct SensorRoute {
    /// Exact MQTT topic to match.
    pub topic: String,
    /// Claim kind to emit.
    pub kind: ClaimKind,
    /// Raw zone label (the host sanitizes/prefixes it).
    pub zone_label: String,
    /// Drop messages whose parsed confidence is below this floor.
    pub min_confidence: f32,
    /// When true, only `state: on`/truthy payloads emit a claim (e.g. contact "opened").
    pub require_truthy_state: bool,
    /// When set, the payload must carry a numeric reading >= this threshold to emit a claim
    /// (bare numeric string, or a numeric/numeric-string `state` field). Replaces the truthy
    /// gate for numeric sensors: occupant counters, HA statestream sensor states, lux values.
    /// The reading itself is gating-only — it never reaches the claim.
    pub numeric_min: Option<f32>,
    /// When set, the gating state is read from this JSON field instead of `state`. For
    /// multiplexed topics — a Canary's `sensing` stream carries `acoustic_event` alongside
    /// cumulative counters — so a route can gate on the one field that means "now" rather
    /// than a counter that stays truthy forever. Fails closed: a payload that is not a JSON
    /// object, or lacks the field, emits nothing. The field value is gating-only.
    pub state_field: Option<String>,
    /// When set, the state (from `state` or `state_field`) must equal this string exactly
    /// (after trimming) to emit a claim. Replaces the truthy gate, the way `numeric_min`
    /// does — for enum-valued fields where "truthy" is meaningless ("smoke_alarm_t3" vs
    /// "none"). The matched value is gating-only — it never reaches the claim.
    pub state_equals: Option<String>,
    /// Provenance declared for this route. `None` → plain adapter attestation;
    /// set to [`Attestation::HaBridged`] on routes fed by an HA
    /// `mqtt_statestream` bridge so the dashboard renders the extra hop honestly.
    pub attestation: Option<Attestation>,
}

impl SensorRoute {
    pub fn new(topic: impl Into<String>, kind: ClaimKind, zone_label: impl Into<String>) -> Self {
        Self {
            topic: topic.into(),
            kind,
            zone_label: zone_label.into(),
            min_confidence: 0.0,
            require_truthy_state: false,
            numeric_min: None,
            state_field: None,
            state_equals: None,
            attestation: None,
        }
    }
}

#[derive(Debug, Deserialize, Default)]
struct SensorPayload {
    confidence: Option<f32>,
    zone: Option<String>,
    state: Option<serde_json::Value>,
}

impl SensorPayload {
    /// The `state` field as a string, for truthy parsing ("on", "open", ...).
    /// An explicit `"state": null` is treated as absent, matching the previous
    /// `Option<String>` deserialization (object-without-state => triggered).
    fn state_str(&self) -> Option<String> {
        match &self.state {
            Some(serde_json::Value::Null) | None => None,
            Some(serde_json::Value::String(s)) => Some(s.clone()),
            Some(v) => Some(v.to_string()),
        }
    }

    /// The `state` field as a number (numeric JSON value or numeric string).
    fn state_num(&self) -> Option<f32> {
        match &self.state {
            Some(serde_json::Value::Number(n)) => n.as_f64().map(|v| v as f32),
            Some(serde_json::Value::String(s)) => s.trim().parse::<f32>().ok(),
            _ => None,
        }
    }
}

/// A `(topic, payload)` message fed to the adapter.
pub type SensorMessage = (String, Vec<u8>);

fn parse_truthy(s: &str) -> bool {
    matches!(
        s.trim().to_lowercase().as_str(),
        "on" | "1" | "true" | "open" | "opened" | "detected" | "active"
    )
}

/// Pure transform shared by the MQTT and webhook adapters: map one `(topic/path, payload)`
/// message to at most one claim using a routing table. No I/O — safe to run in the sandbox.
///
/// Several routes may share a topic (a multiplexed stream gated by `state_field` /
/// `state_equals`); the first route whose gates pass wins, so a gated-out route falls
/// through instead of suppressing its siblings.
pub fn route_message(routes: &[SensorRoute], topic: &str, payload: &[u8]) -> Option<Claim> {
    routes
        .iter()
        .filter(|r| r.topic == topic)
        .find_map(|route| route_one(route, payload))
}

/// Apply one route's gates to one payload.
fn route_one(route: &SensorRoute, payload: &[u8]) -> Option<Claim> {
    // Parse JSON first; `is_json_object` reflects whether parsing actually succeeded, so a
    // malformed payload that merely starts with '{' is NOT treated as a triggered object.
    let raw = std::str::from_utf8(payload).ok()?;
    let (mut parsed, is_json_object) = serde_json::from_str::<SensorPayload>(raw)
        .map(|p| (p, true))
        .unwrap_or((SensorPayload::default(), false));

    // A named state_field redirects the gating state. Fail closed: no JSON object, no
    // field, or an explicit null all emit nothing — a multiplexed stream must never
    // trigger on "the field happened to be absent."
    if let Some(field) = &route.state_field {
        if !is_json_object {
            return None;
        }
        let value: serde_json::Value = serde_json::from_str(raw).ok()?;
        match value.get(field) {
            Some(v) if !v.is_null() => parsed.state = Some(v.clone()),
            _ => return None,
        }
    }

    // Numeric gate: when configured, it replaces the truthy gate entirely. A payload that
    // doesn't parse as a number (bare or in `state`) emits nothing — silence over guessing.
    if let Some(threshold) = route.numeric_min {
        let reading = if is_json_object {
            parsed.state_num()
        } else {
            raw.trim().parse::<f32>().ok()
        };
        match reading {
            Some(v) if v >= threshold => {}
            _ => return None,
        }
    } else if let Some(expected) = &route.state_equals {
        // Exact-match gate for enum-valued fields; replaces the truthy gate the way
        // numeric_min does. Reads `state`/`state_field`, or the bare payload for
        // non-JSON messages. No state at all fails closed.
        let matches = match (parsed.state_str(), is_json_object) {
            (Some(s), _) => s.trim() == expected,
            (None, false) => raw.trim() == expected,
            (None, true) => false,
        };
        if !matches {
            return None;
        }
    } else {
        let state_truthy = match (parsed.state_str(), is_json_object) {
            (Some(s), _) => parse_truthy(&s),
            (None, true) => true, // valid JSON object without explicit state => triggered
            (None, false) => parse_truthy(raw),
        };

        if route.require_truthy_state && !state_truthy {
            return None;
        }
    }

    // Fail closed on a gated route: a payload that omits (or misspells)
    // "confidence" must not sail past the floor as full confidence. Ungated
    // routes keep accepting bare trigger payloads, scored 1.0 as before.
    let confidence = match parsed.confidence {
        Some(confidence) => confidence,
        None if route.min_confidence > 0.0 => return None,
        None => 1.0,
    };
    if confidence < route.min_confidence {
        return None;
    }

    let zone_label = parsed
        .zone
        .clone()
        .unwrap_or_else(|| route.zone_label.clone());
    let mut claim = Claim::new(route.kind, zone_label, confidence);
    if let Some(att) = route.attestation {
        claim = claim.with_attestation(att);
    }
    Some(claim)
}

/// Parse a batch of messages, optionally inside the seccomp sandbox. Shared by both adapters.
pub(crate) fn parse_messages(
    routes: &[SensorRoute],
    msgs: &[SensorMessage],
    sandbox: bool,
) -> Result<Vec<Claim>> {
    let do_parse = || {
        let mut out = Vec::new();
        for (topic, payload) in msgs {
            if let Some(claim) = route_message(routes, topic, payload) {
                out.push(claim);
            }
        }
        Ok(out)
    };
    #[cfg(feature = "adapter-sandbox")]
    {
        if sandbox {
            return crate::adapter::sandbox::parse_in_sandbox(do_parse);
        }
    }
    let _ = sandbox;
    do_parse()
}

/// Generic MQTT sensor adapter.
/// A routing table shared with the host so it can be hot-reloaded without restarting the adapter.
pub type SharedRoutes = std::sync::Arc<std::sync::Mutex<Vec<SensorRoute>>>;

pub struct MqttSensorAdapter {
    rx: Receiver<SensorMessage>,
    routes: SharedRoutes,
    sandbox: bool,
}

impl MqttSensorAdapter {
    /// Build the adapter from a routing table; returns the feeding [`Sender`].
    pub fn new(routes: Vec<SensorRoute>) -> (Self, Sender<SensorMessage>) {
        let (tx, rx) = channel();
        (
            Self {
                rx,
                routes: std::sync::Arc::new(std::sync::Mutex::new(routes)),
                sandbox: false,
            },
            tx,
        )
    }

    /// Opt in to running payload parsing inside the seccomp sandbox (requires the
    /// `adapter-sandbox` feature; no effect otherwise).
    pub fn with_sandbox(mut self, enabled: bool) -> Self {
        self.sandbox = enabled;
        self
    }

    /// Handle to the live routing table, for hot-reload by the host.
    pub fn routes_handle(&self) -> SharedRoutes {
        std::sync::Arc::clone(&self.routes)
    }

    /// Topics this adapter wants subscribed (for the feeder/binary).
    /// Deduplicated: several routes may share one multiplexed topic, and the
    /// feeder should subscribe to it once.
    pub fn topics(&self) -> Vec<String> {
        let mut seen = std::collections::BTreeSet::new();
        self.routes
            .lock()
            .expect("routes mutex")
            .iter()
            .filter(|r| seen.insert(r.topic.clone()))
            .map(|r| r.topic.clone())
            .collect()
    }

    /// Pure transform: map one message to at most one claim, per the routing table.
    pub fn message_to_claim(&self, topic: &str, payload: &[u8]) -> Option<Claim> {
        route_message(&self.routes.lock_tolerant(), topic, payload)
    }
}

impl SensorAdapter for MqttSensorAdapter {
    fn name(&self) -> &'static str {
        "mqtt_sensor_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &MQTT_SENSOR_DESCRIPTOR
    }

    fn poll(&mut self) -> Result<Vec<Claim>> {
        let mut msgs = Vec::new();
        while let Ok(msg) = self.rx.try_recv() {
            msgs.push(msg);
        }
        if msgs.is_empty() {
            return Ok(Vec::new());
        }
        let routes = self.routes.lock_tolerant();
        parse_messages(&routes, &msgs, self.sandbox)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn acoustic_json_payload_maps_to_claim() {
        let routes = vec![SensorRoute::new(
            "sensors/garage/acoustic",
            ClaimKind::AcousticImpulseInZone,
            "garage",
        )];
        let (adapter, _tx) = MqttSensorAdapter::new(routes);
        let claim = adapter
            .message_to_claim("sensors/garage/acoustic", br#"{"confidence":0.8}"#)
            .expect("claim");
        assert_eq!(claim.kind, ClaimKind::AcousticImpulseInZone);
        assert_eq!(claim.zone_label, "garage");
        assert!((claim.confidence - 0.8).abs() < 1e-6);
    }

    #[test]
    fn canary_tamper_payload_maps_to_tamper_claim() {
        // The exact payload shape the Canary firmware publishes on
        // securacv/<id>/tamper (the extra "kind" field is informational
        // for HA subscribers and ignored here).
        let mut route = SensorRoute::new(
            "securacv/canary-1/tamper",
            ClaimKind::TamperDetected,
            "canary_1",
        );
        route.require_truthy_state = true;
        route.min_confidence = 0.5;
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        let claim = adapter
            .message_to_claim(
                "securacv/canary-1/tamper",
                br#"{"state":"on","confidence":0.93,"kind":"enclosure_tamper"}"#,
            )
            .expect("claim");
        assert_eq!(claim.kind, ClaimKind::TamperDetected);
        assert_eq!(claim.zone_label, "canary_1");
        assert!((claim.confidence - 0.93).abs() < 1e-6);
        // Below the confidence floor: dropped.
        assert!(adapter
            .message_to_claim(
                "securacv/canary-1/tamper",
                br#"{"state":"on","confidence":0.2,"kind":"camera_tamper"}"#,
            )
            .is_none());
    }

    #[test]
    fn gated_route_rejects_payload_without_confidence() {
        // A route with a confidence floor must not score a payload that
        // omits (or misspells) "confidence" as full confidence — that would
        // let any publisher bypass the floor by simply not naming the field.
        let mut route = SensorRoute::new(
            "sensors/garage/acoustic",
            ClaimKind::AcousticImpulseInZone,
            "garage",
        );
        route.min_confidence = 0.5;
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        assert!(adapter
            .message_to_claim("sensors/garage/acoustic", br"{}")
            .is_none());
        assert!(adapter
            .message_to_claim("sensors/garage/acoustic", br#"{"confidance":0.9}"#)
            .is_none());
        // Stated confidence above the floor still passes.
        assert!(adapter
            .message_to_claim("sensors/garage/acoustic", br#"{"confidence":0.9}"#)
            .is_some());
    }

    #[test]
    fn ungated_route_still_accepts_payload_without_confidence() {
        // No floor configured: bare trigger payloads (plain "ON", empty JSON)
        // keep working, scored 1.0 as before.
        let (adapter, _tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
            "sensors/garage/acoustic",
            ClaimKind::AcousticImpulseInZone,
            "garage",
        )]);
        let claim = adapter
            .message_to_claim("sensors/garage/acoustic", br"{}")
            .expect("claim");
        assert!((claim.confidence - 1.0).abs() < 1e-6);
    }

    #[test]
    fn contact_requires_truthy_state() {
        let mut route = SensorRoute::new(
            "sensors/door/contact",
            ClaimKind::ContactStateChange,
            "front_door",
        );
        route.require_truthy_state = true;
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        assert!(adapter
            .message_to_claim("sensors/door/contact", b"OFF")
            .is_none());
        assert!(adapter
            .message_to_claim("sensors/door/contact", b"ON")
            .is_some());
    }

    #[test]
    fn unrouted_topic_yields_nothing() {
        let (adapter, _tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
            "a/b",
            ClaimKind::ContactStateChange,
            "z",
        )]);
        assert!(adapter.message_to_claim("x/y", b"ON").is_none());
    }

    #[test]
    fn malformed_json_does_not_trigger_truthy_route() {
        let mut route = SensorRoute::new(
            "sensors/door/contact",
            ClaimKind::ContactStateChange,
            "front_door",
        );
        route.require_truthy_state = true;
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        // Truncated/invalid JSON that starts with '{' must NOT be treated as a triggered object.
        assert!(adapter
            .message_to_claim("sensors/door/contact", br#"{"state":"off"#)
            .is_none());
    }

    #[test]
    fn route_attestation_reaches_the_claim() {
        let mut route = SensorRoute::new(
            "securacv_statestream/binary_sensor/mr60_person/state",
            ClaimKind::PresenceInRestrictedZone,
            "bedroom",
        );
        route.require_truthy_state = true;
        route.attestation = Some(Attestation::HaBridged);
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        let claim = adapter
            .message_to_claim(
                "securacv_statestream/binary_sensor/mr60_person/state",
                b"on",
            )
            .expect("claim");
        assert_eq!(claim.attestation, Some(Attestation::HaBridged));
        // Routes without a declaration leave it to the contract default.
        let (plain, _tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
            "s/pir",
            ClaimKind::PresenceInRestrictedZone,
            "lobby",
        )]);
        assert_eq!(
            plain
                .message_to_claim("s/pir", b"ON")
                .expect("claim")
                .attestation,
            None
        );
    }

    #[test]
    fn explicit_null_state_behaves_like_absent_state() {
        // {"state": null} must keep the pre-Option<Value> behavior: a valid
        // JSON object without a usable state counts as triggered.
        let mut route = SensorRoute::new(
            "sensors/lobby/pir",
            ClaimKind::PresenceInRestrictedZone,
            "lobby",
        );
        route.require_truthy_state = true;
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        assert!(adapter
            .message_to_claim("sensors/lobby/pir", br#"{"state":null,"confidence":0.9}"#)
            .is_some());
    }

    #[test]
    fn numeric_min_gates_bare_numeric_payloads() {
        // HA statestream / ESPHome publish bare states; an mmWave target counter
        // publishes "0".."N". Presence claim only when the count clears the floor.
        let mut route = SensorRoute::new(
            "ha/statestream/sensor/mr60_target_number/state",
            ClaimKind::PresenceInRestrictedZone,
            "bedroom",
        );
        route.numeric_min = Some(1.0);
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        let topic = "ha/statestream/sensor/mr60_target_number/state";
        assert!(adapter.message_to_claim(topic, b"0").is_none());
        assert!(adapter.message_to_claim(topic, b"1").is_some());
        assert!(adapter.message_to_claim(topic, b"2").is_some());
        // Non-numeric payloads emit nothing when the numeric gate is configured.
        assert!(adapter.message_to_claim(topic, b"unavailable").is_none());
        assert!(adapter.message_to_claim(topic, b"on").is_none());
    }

    #[test]
    fn numeric_min_reads_json_state_field() {
        let mut route = SensorRoute::new("s/occupancy", ClaimKind::PresenceInRestrictedZone, "lab");
        route.numeric_min = Some(2.0);
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        // Numeric JSON state and numeric-string state both gate correctly.
        assert!(adapter
            .message_to_claim("s/occupancy", br#"{"state":2}"#)
            .is_some());
        assert!(adapter
            .message_to_claim("s/occupancy", br#"{"state":"3","confidence":0.9}"#)
            .is_some());
        assert!(adapter
            .message_to_claim("s/occupancy", br#"{"state":1}"#)
            .is_none());
        // JSON object without a numeric state emits nothing under the numeric gate.
        assert!(adapter
            .message_to_claim("s/occupancy", br#"{"confidence":0.9}"#)
            .is_none());
    }

    #[test]
    fn payload_zone_override_wins() {
        let (adapter, _tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
            "s/pir",
            ClaimKind::PresenceInRestrictedZone,
            "default_zone",
        )]);
        let claim = adapter
            .message_to_claim("s/pir", br#"{"zone":"server_room"}"#)
            .expect("claim");
        assert_eq!(claim.zone_label, "server_room");
    }

    /// The exact shape the Canary WAP publishes on securacv/<id>/sensing: one
    /// enum-valued `acoustic_event` field ("smoke_alarm_t3" during the 30 s
    /// hold, "none" otherwise) beside *cumulative* detection counters that stay
    /// nonzero forever. Gating must read the enum, never the counters.
    fn wap_sensing_routes() -> Vec<SensorRoute> {
        let mut smoke = SensorRoute::new(
            "securacv/canary-1/sensing",
            ClaimKind::AcousticImpulseInZone,
            "smoke_alarm_heard",
        );
        smoke.state_field = Some("acoustic_event".into());
        smoke.state_equals = Some("smoke_alarm_t3".into());
        let mut co = SensorRoute::new(
            "securacv/canary-1/sensing",
            ClaimKind::AcousticImpulseInZone,
            "co_alarm_heard",
        );
        co.state_field = Some("acoustic_event".into());
        co.state_equals = Some("co_alarm_t4".into());
        vec![smoke, co]
    }

    #[test]
    fn multiplexed_sensing_stream_routes_by_state_field_equality() {
        let routes = wap_sensing_routes();
        let smoke_payload =
            br#"{"acoustic_event":"smoke_alarm_t3","mic_muted":false,"t3_detected":1,"t4_detected":0}"#;
        let claim = route_message(&routes, "securacv/canary-1/sensing", smoke_payload)
            .expect("smoke claim");
        assert_eq!(claim.kind, ClaimKind::AcousticImpulseInZone);
        assert_eq!(claim.zone_label, "smoke_alarm_heard");

        // A gated-out first route must fall through to its sibling, not
        // suppress it: the CO payload reaches the second route.
        let co_payload =
            br#"{"acoustic_event":"co_alarm_t4","mic_muted":false,"t3_detected":1,"t4_detected":1}"#;
        let claim = route_message(&routes, "securacv/canary-1/sensing", co_payload)
            .expect("co claim");
        assert_eq!(claim.zone_label, "co_alarm_heard");
    }

    #[test]
    fn cumulative_counters_never_retrigger_after_the_hold_clears() {
        // The 60 s heartbeat after a past detection: counters are nonzero but
        // the event field is back to "none". No claim — this is the exact
        // false-positive a truthy gate on the counter would produce.
        let routes = wap_sensing_routes();
        let heartbeat =
            br#"{"acoustic_event":"none","mic_muted":false,"t3_detected":5,"t4_detected":2}"#;
        assert!(route_message(&routes, "securacv/canary-1/sensing", heartbeat).is_none());
    }

    #[test]
    fn state_field_fails_closed_on_absent_field_or_non_json() {
        let routes = wap_sensing_routes();
        // Field missing entirely.
        assert!(route_message(&routes, "securacv/canary-1/sensing", br#"{"mic_muted":true}"#)
            .is_none());
        // Explicit null.
        assert!(route_message(
            &routes,
            "securacv/canary-1/sensing",
            br#"{"acoustic_event":null}"#
        )
        .is_none());
        // Not a JSON object at all.
        assert!(route_message(&routes, "securacv/canary-1/sensing", b"smoke_alarm_t3").is_none());
    }

    #[test]
    fn state_equals_reads_bare_payloads_without_a_state_field() {
        let mut route = SensorRoute::new(
            "sensors/porch/mode",
            ClaimKind::ContactStateChange,
            "porch",
        );
        route.state_equals = Some("opened".into());
        let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
        assert!(adapter.message_to_claim("sensors/porch/mode", b"opened").is_some());
        assert!(adapter.message_to_claim("sensors/porch/mode", b"closed").is_none());
        // JSON object without a state field: fail closed, never "triggered".
        assert!(adapter
            .message_to_claim("sensors/porch/mode", br#"{"battery":97}"#)
            .is_none());
    }
}
