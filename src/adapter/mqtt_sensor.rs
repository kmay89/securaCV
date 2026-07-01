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
use crate::adapter::SensorAdapter;
use crate::EventType;

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
}

impl SensorRoute {
    pub fn new(topic: impl Into<String>, kind: ClaimKind, zone_label: impl Into<String>) -> Self {
        Self {
            topic: topic.into(),
            kind,
            zone_label: zone_label.into(),
            min_confidence: 0.0,
            require_truthy_state: false,
        }
    }
}

#[derive(Debug, Deserialize, Default)]
struct SensorPayload {
    confidence: Option<f32>,
    zone: Option<String>,
    state: Option<String>,
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
pub fn route_message(routes: &[SensorRoute], topic: &str, payload: &[u8]) -> Option<Claim> {
    let route = routes.iter().find(|r| r.topic == topic)?;

    // Parse JSON first; `is_json_object` reflects whether parsing actually succeeded, so a
    // malformed payload that merely starts with '{' is NOT treated as a triggered object.
    let raw = std::str::from_utf8(payload).ok()?;
    let (parsed, is_json_object) = serde_json::from_str::<SensorPayload>(raw)
        .map(|p| (p, true))
        .unwrap_or((SensorPayload::default(), false));

    let state_truthy = match (&parsed.state, is_json_object) {
        (Some(s), _) => parse_truthy(s),
        (None, true) => true, // valid JSON object without explicit state => triggered
        (None, false) => parse_truthy(raw),
    };

    if route.require_truthy_state && !state_truthy {
        return None;
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
    Some(Claim::new(route.kind, zone_label, confidence))
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
    pub fn topics(&self) -> Vec<String> {
        self.routes
            .lock()
            .expect("routes mutex")
            .iter()
            .map(|r| r.topic.clone())
            .collect()
    }

    /// Pure transform: map one message to at most one claim, per the routing table.
    pub fn message_to_claim(&self, topic: &str, payload: &[u8]) -> Option<Claim> {
        route_message(&self.routes.lock().expect("routes mutex"), topic, payload)
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
        let routes = self.routes.lock().expect("routes mutex");
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
}
