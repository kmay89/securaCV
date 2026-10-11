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
use crate::adapter::{LockTolerant, SensorAdapter};
use crate::transport::frigate::{
    camera_slug, parse_frigate_event, parse_review_event, select_event_type,
};
use crate::EventType;

/// Default Frigate labels processed when none are configured — the one list
/// `frigate_bridge` reads too ([`crate::transport::frigate::DEFAULT_LABELS`]).
pub const DEFAULT_LABELS: &[&str] = crate::transport::frigate::DEFAULT_LABELS;

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

/// Camera/label/confidence filters, shared with the host so they can be hot-reloaded.
#[derive(Clone)]
pub struct FrigateFilter {
    /// Camera allowlist as slugs ([`camera_slug`], the rule the add-on wizard
    /// writes with), or `None` for every camera.
    pub allowed_cameras: Option<Vec<String>>,
    /// Lowercase label allowlist. An identity label listed here still seals
    /// nothing ([`select_event_type`]).
    pub allowed_labels: Vec<String>,
    pub min_confidence: f64,
}

impl FrigateFilter {
    /// Normalize a filter: camera names become slugs, labels lowercase; empty
    /// labels fall back to [`DEFAULT_LABELS`].
    pub fn new(
        allowed_cameras: Option<Vec<String>>,
        allowed_labels: Vec<String>,
        min_confidence: f64,
    ) -> Self {
        let allowed_labels = if allowed_labels.is_empty() {
            DEFAULT_LABELS.iter().map(|s| s.to_string()).collect()
        } else {
            allowed_labels
                .into_iter()
                .map(|l| l.to_lowercase())
                .collect()
        };
        let allowed_cameras =
            allowed_cameras.map(|cams| cams.iter().map(|c| camera_slug(c)).collect());
        Self {
            allowed_cameras,
            allowed_labels,
            min_confidence,
        }
    }
}

/// Shared, hot-reloadable filter handle.
pub type SharedFrigateFilter = std::sync::Arc<std::sync::Mutex<FrigateFilter>>;

/// Frigate NVR adapter. Construct with [`FrigateAdapter::new`] and feed it via the returned
/// [`Sender`].
pub struct FrigateAdapter {
    rx: Receiver<FrigateMessage>,
    filter: SharedFrigateFilter,
    sandbox: bool,
}

impl FrigateAdapter {
    /// Create a Frigate adapter and its feeding [`Sender`].
    ///
    /// - `allowed_cameras`: if `Some`, only cameras whose [`camera_slug`] equals one of
    ///   these entries' slugs are processed.
    /// - `allowed_labels`: object labels to process (lowercased). Empty falls back to
    ///   [`DEFAULT_LABELS`].
    /// - `min_confidence`: drop detections below this score.
    pub fn new(
        allowed_cameras: Option<Vec<String>>,
        allowed_labels: Vec<String>,
        min_confidence: f64,
    ) -> (Self, Sender<FrigateMessage>) {
        let (tx, rx) = channel();
        let filter = FrigateFilter::new(allowed_cameras, allowed_labels, min_confidence);
        (
            Self {
                rx,
                filter: std::sync::Arc::new(std::sync::Mutex::new(filter)),
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

    /// Handle to the live filter, for hot-reload by the host.
    pub fn filter_handle(&self) -> SharedFrigateFilter {
        std::sync::Arc::clone(&self.filter)
    }

    /// Pure transform: parse one Frigate payload into zero or more claims, applying the
    /// camera/label/confidence filters. Reuses `transport::frigate`.
    pub fn parse_to_claims(&self, topic: &str, payload: &[u8]) -> Result<Vec<Claim>> {
        let filter = self.filter.lock_tolerant();
        parse_with_filter(&filter, topic, payload)
    }
}

/// Pure, I/O-free transform applying a [`FrigateFilter`] (safe to run in the sandbox).
fn parse_with_filter(filter: &FrigateFilter, topic: &str, payload: &[u8]) -> Result<Vec<Claim>> {
    let parsed = if topic.contains("/reviews") {
        parse_review_event(payload)?
    } else {
        parse_frigate_event(payload)?
    };

    if let Some(allowed) = &filter.allowed_cameras {
        if !allowed.contains(&camera_slug(&parsed.camera)) {
            return Ok(vec![]);
        }
    }

    // No label survived the allowlist (or only identity labels were named):
    // nothing to seal.
    let Some(event_type) = select_event_type(&parsed.labels, &filter.allowed_labels) else {
        return Ok(vec![]);
    };

    if parsed.confidence < filter.min_confidence {
        return Ok(vec![]);
    }

    let kind = match event_type {
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

impl SensorAdapter for FrigateAdapter {
    fn name(&self) -> &'static str {
        "frigate_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &FRIGATE_DESCRIPTOR
    }

    fn poll(&mut self) -> Result<Vec<Claim>> {
        let _ = self.sandbox; // read unconditionally; only consulted under `adapter-sandbox`.
        let mut msgs = Vec::new();
        while let Ok(msg) = self.rx.try_recv() {
            msgs.push(msg);
        }
        if msgs.is_empty() {
            return Ok(Vec::new());
        }
        // Snapshot the filter so we don't hold the lock across the sandbox fork.
        let filter = self.filter.lock_tolerant().clone();
        let parse_all = || {
            let mut out = Vec::new();
            for (topic, payload) in &msgs {
                match parse_with_filter(&filter, topic, payload) {
                    Ok(mut claims) => out.append(&mut claims),
                    Err(e) => log::debug!("frigate adapter skipped payload: {}", e),
                }
            }
            Ok(out)
        };
        #[cfg(feature = "adapter-sandbox")]
        {
            if self.sandbox {
                return crate::adapter::sandbox::parse_in_sandbox(parse_all);
            }
        }
        parse_all()
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

    #[test]
    fn camera_allowlist_compares_slugs() {
        // "Front Door" in the allowlist names the camera Frigate calls "front_door".
        let (adapter, _tx) = FrigateAdapter::new(Some(vec!["Front Door".to_string()]), vec![], 0.5);
        let claims = adapter
            .parse_to_claims("frigate/events", EVENT_PERSON.as_bytes())
            .expect("parse");
        assert_eq!(claims.len(), 1);

        let (adapter, _tx) = FrigateAdapter::new(Some(vec!["garage".to_string()]), vec![], 0.5);
        let claims = adapter
            .parse_to_claims("frigate/events", EVENT_PERSON.as_bytes())
            .expect("parse");
        assert!(claims.is_empty());
    }

    #[test]
    fn identity_labels_never_become_claims_even_when_allowlisted() {
        let payload = r#"{"after":{"camera":"front_door","label":"face","top_score":0.99,
            "entered_zones":["porch"]},"type":"new"}"#;
        let (adapter, _tx) =
            FrigateAdapter::new(None, vec!["face".to_string(), "person".to_string()], 0.5);
        // The parser refuses the label outright: an error, never a claim.
        assert!(adapter
            .parse_to_claims("frigate/events", payload.as_bytes())
            .is_err());
    }

    #[test]
    fn review_alert_becomes_one_claim_at_full_confidence() {
        let payload = r#"{"type":"new","before":null,"after":{"camera":"front_door",
            "severity":"alert","data":{"objects":["dog","person"],"zones":["porch"]}}}"#;
        let (adapter, _tx) = FrigateAdapter::new(None, vec![], 0.5);
        let claims = adapter
            .parse_to_claims("frigate/reviews", payload.as_bytes())
            .expect("parse");
        assert_eq!(claims.len(), 1);
        assert_eq!(claims[0].kind, ClaimKind::LargeObjectBoundaryCrossing);
        assert_eq!(claims[0].zone_label, "porch");
        assert!((claims[0].confidence - 1.0).abs() < f32::EPSILON);

        let detection = payload.replace("\"severity\":\"alert\"", "\"severity\":\"detection\"");
        assert!(adapter
            .parse_to_claims("frigate/reviews", detection.as_bytes())
            .is_err());
    }
}
