//! Frigate MQTT event parsing utilities.
//!
//! This module provides shared parsing logic for Frigate NVR events,
//! used by both the frigate_bridge binary and integration tests.

use crate::EventType;
use anyhow::{anyhow, Result};
use serde::Deserialize;

/// Frigate event from MQTT - handles the nested before/after format.
/// Frigate publishes: `{ "before": {...}, "after": {...}, "type": "new"|"update"|"end" }`
#[derive(Debug, Deserialize)]
pub struct FrigateEventWrapper {
    /// The "after" state contains current detection info
    pub after: Option<FrigateEventData>,

    /// Event type: "new", "update", or "end"
    #[serde(rename = "type")]
    pub event_type: Option<String>,
}

/// Inner event data from Frigate (the "after" section).
#[derive(Debug, Deserialize)]
pub struct FrigateEventData {
    /// Camera name (mapped to zone_id)
    pub camera: String,

    /// Object label (person, car, dog, etc.)
    pub label: String,

    /// Sub-label for more specific classification (e.g., "amazon" for package)
    #[serde(default)]
    pub sub_label: Option<String>,

    /// Detection confidence (0.0-1.0)
    #[serde(default)]
    pub score: f64,

    /// Top score seen for this object (use this if available)
    pub top_score: Option<f64>,

    /// Current zones the object is in
    #[serde(default)]
    pub current_zones: Vec<String>,

    /// All zones the object has entered during tracking
    #[serde(default)]
    pub entered_zones: Vec<String>,

    /// Whether this is a false positive (skip if true)
    #[serde(default)]
    pub false_positive: bool,
}

/// Frigate review event (alternative topic for confirmations).
#[derive(Debug, Deserialize)]
pub struct FrigateReview {
    /// Type: "new", "update", or "end"
    #[serde(rename = "type")]
    pub review_type: Option<String>,

    /// Camera name
    pub camera: String,

    /// Detection data
    pub data: Option<FrigateReviewData>,
}

/// Review data from Frigate.
#[derive(Debug, Deserialize)]
pub struct FrigateReviewData {
    /// Object detections in this review
    #[serde(default)]
    pub objects: Vec<String>,

    /// Detection score
    #[serde(default)]
    pub score: Option<f64>,

    /// Zones involved
    #[serde(default)]
    pub zones: Vec<String>,
}

/// Parsed Frigate event data.
#[derive(Debug, Clone)]
pub struct ParsedFrigateEvent {
    /// Camera name
    pub camera: String,
    /// Object label (with sub_label if present)
    pub label: String,
    /// Detection confidence (uses top_score if available)
    pub confidence: f64,
    /// Zones the object is in or has entered
    pub zones: Vec<String>,
}

/// Parse a Frigate event JSON payload into components.
///
/// Returns an error if:
/// - The JSON is malformed
/// - The event type is "update" or "end" (to prevent duplicate logging)
/// - The event is marked as a false positive
/// - The "after" section is missing
pub fn parse_frigate_event(payload: &[u8]) -> Result<ParsedFrigateEvent> {
    let wrapper: FrigateEventWrapper =
        serde_json::from_slice(payload).map_err(|e| anyhow!("parse error: {}", e))?;

    // Only process "new" events (not "update" or "end")
    // This prevents duplicate logging for the same detection
    match wrapper.event_type.as_deref() {
        Some("new") => {}
        Some("end") => return Err(anyhow!("event ended, already processed")),
        Some("update") => return Err(anyhow!("update event, already processed")),
        _ => {} // Accept if no type specified (backward compat)
    }

    let event = wrapper
        .after
        .ok_or_else(|| anyhow!("missing 'after' section in event"))?;

    if event.false_positive {
        return Err(anyhow!("false positive event"));
    }

    let confidence = event.top_score.unwrap_or(event.score);

    // Combine sub_label with label if present (e.g., "package:amazon")
    let label = match &event.sub_label {
        Some(sub) if !sub.is_empty() => format!("{}:{}", event.label, sub),
        _ => event.label.clone(),
    };

    // Prefer entered_zones over current_zones for more complete zone coverage
    let zones = if !event.entered_zones.is_empty() {
        event.entered_zones.clone()
    } else {
        event.current_zones.clone()
    };

    Ok(ParsedFrigateEvent {
        camera: event.camera,
        label,
        confidence,
        zones,
    })
}

/// Parse a Frigate review event JSON payload.
pub fn parse_review_event(payload: &[u8]) -> Result<ParsedFrigateEvent> {
    let review: FrigateReview =
        serde_json::from_slice(payload).map_err(|e| anyhow!("parse error: {}", e))?;

    // Only process "new" reviews
    if review.review_type.as_deref() != Some("new") {
        return Err(anyhow!("not a new review"));
    }

    let data = review.data.ok_or_else(|| anyhow!("no review data"))?;
    let label = data
        .objects
        .first()
        .cloned()
        .unwrap_or_else(|| "unknown".to_string());
    let confidence = data.score.unwrap_or(0.5);

    Ok(ParsedFrigateEvent {
        camera: review.camera,
        label,
        confidence,
        zones: data.zones,
    })
}

/// Map a Frigate object label to a PWK EventType.
pub fn map_label_to_event_type(label: &str) -> EventType {
    // Handle sub_label format (e.g., "package:amazon" -> use "package")
    let base_label = label.split(':').next().unwrap_or(label);

    match base_label.to_lowercase().as_str() {
        "person" | "face" => EventType::BoundaryCrossingObjectLarge,
        "car" | "truck" | "bus" | "motorcycle" => EventType::BoundaryCrossingObjectLarge,
        "dog" | "cat" | "bird" | "animal" => EventType::BoundaryCrossingObjectSmall,
        "bicycle" | "skateboard" => EventType::BoundaryCrossingObjectSmall,
        "package" | "box" => EventType::BoundaryCrossingObjectSmall,
        _ => EventType::BoundaryCrossingObjectSmall,
    }
}

/// Sanitize a zone name for use as a zone_id.
///
/// - Converts to lowercase
/// - Replaces non-alphanumeric characters (except '_' and '-') with '_'
/// - Limits length to 64 characters
pub fn sanitize_zone_name(name: &str) -> String {
    name.to_lowercase()
        .chars()
        .map(|c| {
            if c.is_alphanumeric() || c == '_' || c == '-' {
                c
            } else {
                '_'
            }
        })
        .take(64)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const FRIGATE_EVENT_NEW: &str = r#"{
        "before": null,
        "after": {
            "id": "1234567890.abc123",
            "camera": "front_door",
            "label": "person",
            "sub_label": null,
            "score": 0.75,
            "top_score": 0.92,
            "current_zones": ["porch"],
            "entered_zones": ["driveway", "porch"],
            "false_positive": false
        },
        "type": "new"
    }"#;

    #[test]
    fn parse_new_event_succeeds() {
        let result = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes());
        assert!(result.is_ok());

        let event = result.unwrap();
        assert_eq!(event.camera, "front_door");
        assert_eq!(event.label, "person");
        assert!((event.confidence - 0.92).abs() < 0.001);
        assert_eq!(event.zones, vec!["driveway", "porch"]);
    }

    #[test]
    fn parse_update_event_rejected() {
        let payload = r#"{"after": {"camera": "x", "label": "y"}, "type": "update"}"#;
        let result = parse_frigate_event(payload.as_bytes());
        assert!(result.is_err());
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("already processed"));
    }

    #[test]
    fn map_person_to_large() {
        assert_eq!(
            map_label_to_event_type("person"),
            EventType::BoundaryCrossingObjectLarge
        );
    }

    #[test]
    fn map_dog_to_small() {
        assert_eq!(
            map_label_to_event_type("dog"),
            EventType::BoundaryCrossingObjectSmall
        );
    }

    #[test]
    fn sanitize_removes_spaces() {
        assert_eq!(sanitize_zone_name("Front Door"), "front_door");
    }

    #[test]
    fn sanitize_limits_length() {
        let long_name = "a".repeat(100);
        assert_eq!(sanitize_zone_name(&long_name).len(), 64);
    }
}
