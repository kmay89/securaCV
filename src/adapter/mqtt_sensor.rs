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
    ],
    allowed_event_types: &[
        EventType::BoundaryCrossingObjectLarge,
        EventType::BoundaryCrossingObjectSmall,
        EventType::AcousticImpulseInZone,
        EventType::PresenceInRestrictedZone,
        EventType::VehiclePresenceAfterHours,
        EventType::ContactStateChange,
        EventType::ObjectRemovedFromZone,
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
    pub fn new(
        topic: impl Into<String>,
        kind: ClaimKind,
        zone_label: impl Into<String>,
    ) -> Self {
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

/// Generic MQTT sensor adapter.
pub struct MqttSensorAdapter {
    rx: Receiver<SensorMessage>,
    routes: Vec<SensorRoute>,
}

impl MqttSensorAdapter {
    /// Build the adapter from a routing table; returns the feeding [`Sender`].
    pub fn new(routes: Vec<SensorRoute>) -> (Self, Sender<SensorMessage>) {
        let (tx, rx) = channel();
        (Self { rx, routes }, tx)
    }

    /// Topics this adapter wants subscribed (for the feeder/binary).
    pub fn topics(&self) -> Vec<String> {
        self.routes.iter().map(|r| r.topic.clone()).collect()
    }

    fn parse_truthy(s: &str) -> bool {
        matches!(
            s.trim().to_lowercase().as_str(),
            "on" | "1" | "true" | "open" | "opened" | "detected" | "active"
        )
    }

    /// Pure transform: map one message to at most one claim, per the routing table.
    pub fn message_to_claim(&self, topic: &str, payload: &[u8]) -> Option<Claim> {
        let route = self.routes.iter().find(|r| r.topic == topic)?;

        // Try JSON first; fall back to a bare-string interpretation.
        let raw = std::str::from_utf8(payload).ok()?;
        let parsed: SensorPayload = serde_json::from_str(raw).unwrap_or_default();

        let is_json_object = raw.trim_start().starts_with('{');
        let state_truthy = match (&parsed.state, is_json_object) {
            (Some(s), _) => Self::parse_truthy(s),
            (None, true) => true, // JSON object without explicit state => triggered
            (None, false) => Self::parse_truthy(raw),
        };

        if route.require_truthy_state && !state_truthy {
            return None;
        }

        let confidence = parsed.confidence.unwrap_or(1.0);
        if confidence < route.min_confidence {
            return None;
        }

        let zone_label = parsed.zone.clone().unwrap_or_else(|| route.zone_label.clone());
        Some(Claim::new(route.kind, zone_label, confidence))
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
        let mut out = Vec::new();
        while let Ok((topic, payload)) = self.rx.try_recv() {
            if let Some(claim) = self.message_to_claim(&topic, &payload) {
                out.push(claim);
            }
        }
        Ok(out)
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
        let (adapter, _tx) =
            MqttSensorAdapter::new(vec![SensorRoute::new("a/b", ClaimKind::ContactStateChange, "z")]);
        assert!(adapter.message_to_claim("x/y", b"ON").is_none());
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
