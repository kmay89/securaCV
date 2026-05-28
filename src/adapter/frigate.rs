//! Frigate NVR reference adapter.
//!
//! This is the canonical [`frigate_bridge`](../../bin/frigate_bridge.rs) pattern expressed as a
//! reusable [`SensorAdapter`]. It reuses the existing `transport::frigate` parsers/sanitizers and
//! produces vendor-neutral [`Claim`]s; the [`AdapterHost`](crate::adapter::AdapterHost) performs
//! bucketing, dedup, zone sanitization, and sealing.
//!
//! Decoupling from the network: the adapter holds the receiving end of a channel. A feeder (the
//! `adapter_host` binary's MQTT thread, or a test) pushes `(topic, payload)` pairs into the
//! [`Sender`] returned at construction. [`poll`](FrigateAdapter::poll) drains whatever has
//! arrived and parses it. This keeps the adapter free of any I/O of its own.

use std::sync::mpsc::{channel, Receiver, Sender};

use anyhow::Result;

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::SensorAdapter;
use crate::transport::frigate::{map_label_to_event_type, parse_frigate_event, parse_review_event};
use crate::EventType;

/// Default Frigate labels processed when none are configured (matches `frigate_bridge`).
pub const DEFAULT_LABELS: &[&str] = &[
    "person",
    "car",
    "dog",
    "cat",
    "bird",
    "bicycle",
    "motorcycle",
];

static FRIGATE_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "frigate_adapter",
    allowed_claim_kinds: &[
        ClaimKind::LargeObjectBoundaryCrossing,
        ClaimKind::SmallObjectBoundaryCrossing,
    ],
    allowed_event_types: &[
        EventType::BoundaryCrossingObjectLarge,
        EventType::BoundaryCrossingObjectSmall,
    ],
    requested_capabilities: &[],
};

/// A `(topic, payload)` message fed to the adapter.
pub type FrigateMessage = (String, Vec<u8>);

/// Frigate NVR adapter. Construct with [`FrigateAdapter::new`] and feed it via the returned
/// [`Sender`].
pub struct FrigateAdapter {
    rx: Receiver<FrigateMessage>,
    allowed_cameras: Option<Vec<String>>,
    allowed_labels: Vec<String>,
    min_confidence: f64,
}

impl FrigateAdapter {
    /// Create a Frigate adapter and its feeding [`Sender`].
    ///
    /// - `allowed_cameras`: if `Some`, only these (lowercased) camera names are processed.
    /// - `allowed_labels`: object labels to process (lowercased). Empty falls back to
    ///   [`DEFAULT_LABELS`].
    /// - `min_confidence`: drop detections below this score.
    pub fn new(
        allowed_cameras: Option<Vec<String>>,
        allowed_labels: Vec<String>,
        min_confidence: f64,
    ) -> (Self, Sender<FrigateMessage>) {
        let (tx, rx) = channel();
        let allowed_labels = if allowed_labels.is_empty() {
            DEFAULT_LABELS.iter().map(|s| s.to_string()).collect()
        } else {
            allowed_labels
                .into_iter()
                .map(|l| l.to_lowercase())
                .collect()
        };
        let allowed_cameras =
            allowed_cameras.map(|cams| cams.into_iter().map(|c| c.to_lowercase()).collect());
        (
            Self {
                rx,
                allowed_cameras,
                allowed_labels,
                min_confidence,
            },
            tx,
        )
    }

    /// Pure transform: parse one Frigate payload into zero or more claims, applying the
    /// camera/label/confidence filters. Reuses `transport::frigate`.
    pub fn parse_to_claims(&self, topic: &str, payload: &[u8]) -> Result<Vec<Claim>> {
        let parsed = if topic.contains("/reviews") {
            parse_review_event(payload)?
        } else {
            parse_frigate_event(payload)?
        };

        if let Some(allowed) = &self.allowed_cameras {
            if !allowed.contains(&parsed.camera.to_lowercase()) {
                return Ok(vec![]);
            }
        }

        if !self.allowed_labels.contains(&parsed.label.to_lowercase()) {
            return Ok(vec![]);
        }

        if parsed.confidence < self.min_confidence {
            return Ok(vec![]);
        }

        let kind = match map_label_to_event_type(&parsed.label) {
            EventType::BoundaryCrossingObjectLarge => ClaimKind::LargeObjectBoundaryCrossing,
            // Everything else Frigate maps to a "small" crossing.
            _ => ClaimKind::SmallObjectBoundaryCrossing,
        };

        // Prefer the first Frigate zone; fall back to the camera name. The host sanitizes it.
        let zone_label = parsed
            .zones
            .first()
            .cloned()
            .unwrap_or_else(|| parsed.camera.clone());

        Ok(vec![Claim::new(kind, zone_label, parsed.confidence as f32)])
    }
}

impl SensorAdapter for FrigateAdapter {
    fn name(&self) -> &'static str {
        "frigate_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &FRIGATE_DESCRIPTOR
    }

    fn poll(&mut self) -> Result<Vec<Claim>> {
        let mut out = Vec::new();
        while let Ok((topic, payload)) = self.rx.try_recv() {
            match self.parse_to_claims(&topic, &payload) {
                Ok(mut claims) => out.append(&mut claims),
                Err(e) => log::debug!("frigate adapter skipped payload: {}", e),
            }
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const EVENT_PERSON: &str = r#"{
        "after": {"camera":"front_door","label":"person","score":0.9,"top_score":0.92,
                  "entered_zones":["porch"],"false_positive":false},
        "type":"new"
    }"#;

    #[test]
    fn person_event_becomes_large_crossing_claim() {
        let (adapter, _tx) = FrigateAdapter::new(None, vec![], 0.5);
        let claims = adapter
            .parse_to_claims("frigate/events", EVENT_PERSON.as_bytes())
            .expect("parse");
        assert_eq!(claims.len(), 1);
        assert_eq!(claims[0].kind, ClaimKind::LargeObjectBoundaryCrossing);
        assert_eq!(claims[0].zone_label, "porch");
    }

    #[test]
    fn poll_drains_fed_messages() {
        let (mut adapter, tx) = FrigateAdapter::new(None, vec![], 0.5);
        tx.send((
            "frigate/events".to_string(),
            EVENT_PERSON.as_bytes().to_vec(),
        ))
        .unwrap();
        let claims = adapter.poll().expect("poll");
        assert_eq!(claims.len(), 1);
        // Drained: next poll is empty.
        assert!(adapter.poll().expect("poll2").is_empty());
    }

    #[test]
    fn low_confidence_is_dropped() {
        let (adapter, _tx) = FrigateAdapter::new(None, vec![], 0.95);
        let claims = adapter
            .parse_to_claims("frigate/events", EVENT_PERSON.as_bytes())
            .expect("parse");
        assert!(claims.is_empty());
    }
}
