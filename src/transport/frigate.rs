//! Frigate MQTT event parsing utilities.
//!
//! Shared by the `frigate_bridge` binary, the `adapter::frigate` adapter
//! (feature `adapter-frigate`) and the integration tests. Everything here is
//! a pure transform over bytes: no I/O, no clock, no kernel handle.
//!
//! # Identity stripping (Invariant II, spec/invariants.md)
//!
//! Frigate's `frigate/events` and `frigate/reviews` payloads carry far more
//! than a coarse claim: recognized face names and plate aliases
//! (`sub_label`, `recognized_license_plate`), per-frame boxes and a
//! trajectory with timestamps (`box`, `region`, `path_data`), precise
//! timestamps (`start_time`, `frame_time`), a stable object id that embeds
//! one, a base64 thumbnail, and — from Frigate 0.17 — free-text GenAI review
//! summaries. The deserializers in this module keep ONLY the fields a sealed
//! claim is built from ([`KEPT_EVENT_FIELDS`], [`KEPT_REVIEW_FIELDS`]) and
//! parse everything else as absent: unknown keys are ignored by serde (no
//! struct in this module may ever be `deny_unknown_fields`), and the kept
//! fields accept `null` where Frigate may send it, so a payload is never
//! dropped because of a field the kernel does not want. Dropping the whole
//! message would be the inverse failure — an event with a recognized face
//! would vanish from the log instead of being sealed without the name.
//!
//! The discard lists ([`DISCARDED_EVENT_FIELDS`], [`DISCARDED_REVIEW_FIELDS`])
//! are data, not code, so a test can prove the fixtures exercise every one
//! of them: `tests/frigate_integration.rs` populates each with a sentinel
//! and asserts no sentinel reaches the sealed payload, the export, or the
//! bridge's MQTT output.
//!
//! Two labels are refused outright rather than mapped — [`IDENTITY_LABELS`]
//! (`face`, `license_plate`): an object whose class *is* an identity has no
//! coarse claim to seal, and an operator cannot opt into sealing every plate
//! read through the label allowlist (AGENTS.md rule 1: `ObjectClass` is never
//! `Face` or `LicensePlate`).

use crate::EventType;
use anyhow::{anyhow, Result};
use serde::{Deserialize, Deserializer};

/// `frigate/events` fields a sealed claim is built from — the only ones the
/// deserializer keeps (plus the message-level `type`).
pub const KEPT_EVENT_FIELDS: &[&str] = &[
    "camera",
    "label",
    "score",
    "top_score",
    "current_zones",
    "entered_zones",
    "false_positive",
];

/// `frigate/events` fields discarded at parse time (Frigate 0.13 through
/// 0.17). Every entry is parsed as absent whatever its shape: `sub_label` is
/// `null` or a `[name, score]` pair (never a bare string on this topic),
/// `attributes` is a dict of recognized labels, `path_data` a trajectory
/// with timestamps, `snapshot` a nested object, `thumbnail` a base64 JPEG.
/// Any key not named here or in [`KEPT_EVENT_FIELDS`] is discarded too.
pub const DISCARDED_EVENT_FIELDS: &[&str] = &[
    "sub_label",
    "recognized_license_plate",
    "recognized_license_plate_score",
    "attributes",
    "current_attributes",
    "path_data",
    "box",
    "region",
    "area",
    "ratio",
    "average_estimated_speed",
    "velocity_angle",
    "current_estimated_speed",
    "max_estimated_speed",
    "id",
    "frame_time",
    "start_time",
    "end_time",
    "snapshot_time",
    "snapshot",
    "thumbnail",
    "has_snapshot",
    "has_clip",
    "motionless_count",
    "position_changes",
    "stationary",
    "active",
    "pending_loitering",
    "model_hash",
    "model_type",
    "detector_type",
];

/// `frigate/reviews` fields a sealed claim is built from (plus the
/// message-level `type`; `before.severity` is read only to detect the
/// detection → alert transition). Dotted names are nested under `data`.
pub const KEPT_REVIEW_FIELDS: &[&str] = &["camera", "severity", "data.objects", "data.zones"];

/// `frigate/reviews` fields discarded at parse time: the review id,
/// precise times, the thumbnail's device path, the tracked-object ids,
/// recognized names, audio labels, and the 0.17 GenAI review summary
/// (`metadata`, `description`, `title`, `scene`, `other_concerns` — free
/// text that names people, plates and clothing). Dotted names are nested
/// under `data`; any unlisted key is discarded too.
pub const DISCARDED_REVIEW_FIELDS: &[&str] = &[
    "id",
    "start_time",
    "end_time",
    "thumb_path",
    "data.detections",
    "data.sub_labels",
    "data.audio",
    "metadata",
    "description",
    "title",
    "scene",
    "other_concerns",
];

/// Object labels that name an identity rather than an object class. A
/// tracked object with one of these labels yields no event, and a review's
/// object list is filtered of them before the claim is built.
pub const IDENTITY_LABELS: &[&str] = &["face", "license_plate"];

/// Object labels sealed when the operator configures no allowlist — shared
/// by `frigate_bridge` and the `adapter::frigate` adapter so the two never
/// disagree about the default.
pub const DEFAULT_LABELS: &[&str] = &[
    "person",
    "car",
    "dog",
    "cat",
    "bird",
    "bicycle",
    "motorcycle",
];

/// Whether `label` is one of [`IDENTITY_LABELS`] (ASCII case-insensitive).
pub fn is_identity_label(label: &str) -> bool {
    IDENTITY_LABELS
        .iter()
        .any(|identity| label.eq_ignore_ascii_case(identity))
}

/// Accept `null` for a kept collection field as "empty" instead of failing
/// the whole message (Frigate may null a list it has not populated yet).
fn null_as_default<'de, D, T>(deserializer: D) -> std::result::Result<T, D::Error>
where
    D: Deserializer<'de>,
    T: Default + Deserialize<'de>,
{
    Ok(Option::<T>::deserialize(deserializer)?.unwrap_or_default())
}

/// Frigate event from MQTT - handles the nested before/after format.
/// Frigate publishes: `{ "before": {...}, "after": {...}, "type": "new"|"update"|"end" }`
///
/// `before` is never read: a tracked object is sealed at `new`, and the
/// prior state carries nothing the claim needs.
#[derive(Debug, Deserialize)]
pub struct FrigateEventWrapper {
    /// The "after" state contains current detection info
    pub after: Option<FrigateEventData>,

    /// Event type: "new", "update", or "end"
    #[serde(rename = "type", default)]
    pub event_type: Option<String>,
}

/// Inner event data from Frigate (the "after" section), reduced to
/// [`KEPT_EVENT_FIELDS`]. Every other key Frigate sends — see
/// [`DISCARDED_EVENT_FIELDS`] — is ignored by serde regardless of its shape.
/// This struct must never carry `deny_unknown_fields`: the fixture test
/// that populates every discarded field would fail the build if it did.
#[derive(Debug, Deserialize)]
pub struct FrigateEventData {
    /// Camera name (mapped to zone_id when the object entered no zone)
    pub camera: String,

    /// Object label (person, car, dog, etc.). The base label only — Frigate's
    /// `sub_label` (a recognized name or delivery logo) is discarded, never
    /// folded in.
    pub label: String,

    /// Detection confidence (0.0-1.0)
    #[serde(default)]
    pub score: Option<f64>,

    /// Top score seen for this object (preferred when present)
    #[serde(default)]
    pub top_score: Option<f64>,

    /// Current zones the object is in
    #[serde(default, deserialize_with = "null_as_default")]
    pub current_zones: Vec<String>,

    /// All zones the object has entered during tracking
    #[serde(default, deserialize_with = "null_as_default")]
    pub entered_zones: Vec<String>,

    /// Whether this is a false positive (skipped when true)
    #[serde(default)]
    pub false_positive: Option<bool>,
}

/// Frigate review message (`frigate/reviews`, Frigate 0.14+): a change feed
/// `{ "type": "new"|"update"|"end", "before": {...}, "after": {...} }` where
/// each item carries `camera`, `severity` ("alert" | "detection") and a
/// `data` block with `objects`/`zones`.
#[derive(Debug, Deserialize)]
pub struct FrigateReview {
    /// Type: "new", "update", or "end"
    #[serde(rename = "type", default)]
    pub review_type: Option<String>,

    /// The prior state. Only its severity is read, to seal an alert exactly
    /// once: at `new` when it is already an alert, else at the `update`
    /// that promotes it.
    #[serde(default)]
    pub before: Option<FrigateReviewBefore>,

    /// The current state.
    #[serde(default)]
    pub after: Option<FrigateReviewItem>,
}

/// The `before` item of a review message, reduced to the one field the
/// transition rule needs.
#[derive(Debug, Deserialize)]
pub struct FrigateReviewBefore {
    /// Review severity before this message: "alert" or "detection"
    #[serde(default)]
    pub severity: Option<String>,
}

/// The `after` item of a review message, reduced to [`KEPT_REVIEW_FIELDS`].
#[derive(Debug, Deserialize)]
pub struct FrigateReviewItem {
    /// Camera name
    pub camera: String,

    /// Review severity: "alert" or "detection"
    #[serde(default)]
    pub severity: Option<String>,

    /// Detection data
    #[serde(default)]
    pub data: Option<FrigateReviewData>,
}

/// Review data from Frigate, reduced to the object classes and zones.
/// `detections` (object ids), `sub_labels` (names) and `audio` are
/// discarded — see [`DISCARDED_REVIEW_FIELDS`].
#[derive(Debug, Deserialize)]
pub struct FrigateReviewData {
    /// Object classes in this review
    #[serde(default, deserialize_with = "null_as_default")]
    pub objects: Vec<String>,

    /// Zones involved
    #[serde(default, deserialize_with = "null_as_default")]
    pub zones: Vec<String>,
}

/// Which Frigate message a parsed claim came from. Decides the bridge's
/// dedup key: a tracked object dedupes per camera + base label + bucket,
/// a review alert per camera + bucket.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrigateClaimKind {
    /// A tracked object from `<prefix>/events` at its `new` message.
    TrackedObject,
    /// A review item from `<prefix>/reviews` at the message that made it an alert.
    ReviewAlert,
}

/// Parsed Frigate event data: the coarse claim and nothing else.
#[derive(Debug, Clone, PartialEq)]
pub struct ParsedFrigateEvent {
    /// Camera name
    pub camera: String,
    /// Base object labels the claim is built from: exactly one for a tracked
    /// object; every object class a review alert names, identity labels
    /// already removed. Never empty.
    pub labels: Vec<String>,
    /// Detection confidence (uses top_score if available; `1.0` for a review
    /// alert, which carries no score)
    pub confidence: f64,
    /// Zones the object is in or has entered
    pub zones: Vec<String>,
    /// Which message kind produced this claim.
    pub kind: FrigateClaimKind,
}

/// Which Frigate topic the bridge seals from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrigateMode {
    /// `<prefix>/events`: seal every tracked object at `new` (the default).
    Events,
    /// `<prefix>/reviews`: seal only review items of severity `alert`.
    Reviews,
}

impl FrigateMode {
    /// The wire word for this mode (the `mode` field of `witness/frigate_link`).
    pub fn as_str(self) -> &'static str {
        match self {
            FrigateMode::Events => "events",
            FrigateMode::Reviews => "reviews",
        }
    }
}

impl std::fmt::Display for FrigateMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// The exact MQTT subscriptions the bridge makes for a mode: one topic,
/// `<prefix>/events` or `<prefix>/reviews`. `legacy_events_topic` is the
/// deprecated full-topic override (`--frigate-topic`), honored in events
/// mode only. Nothing else is ever subscribed — not `<camera>/<label>/snapshot`
/// (JPEG bytes), not `tracked_object_update` (names and descriptions), not
/// `camera_activity` or `<camera>/audio/*`.
pub fn frigate_subscriptions(
    mode: FrigateMode,
    topic_prefix: &str,
    legacy_events_topic: Option<&str>,
) -> Vec<String> {
    match mode {
        FrigateMode::Events => vec![legacy_events_topic
            .map(str::to_string)
            .unwrap_or_else(|| format!("{topic_prefix}/events"))],
        FrigateMode::Reviews => vec![format!("{topic_prefix}/reviews")],
    }
}

/// Parse a Frigate event JSON payload into components.
///
/// Returns an error if:
/// - The JSON is malformed, or a kept field has an impossible type
/// - The event type is "update" or "end" (to prevent duplicate logging)
/// - The event is marked as a false positive
/// - The "after" section is missing
/// - The label is an identity label ([`IDENTITY_LABELS`])
///
/// Every discarded field (see [`DISCARDED_EVENT_FIELDS`]) is accepted in any
/// shape and never reaches the result.
pub fn parse_frigate_event(payload: &[u8]) -> Result<ParsedFrigateEvent> {
    let wrapper: FrigateEventWrapper =
        serde_json::from_slice(payload).map_err(|e| anyhow!("parse error: {}", e))?;

    // Only process "new" events (not "update" or "end")
    // This prevents duplicate logging for the same detection
    match wrapper.event_type.as_deref() {
        Some("new") => {}
        Some("end") => return Err(anyhow!("event ended, already processed")),
        Some("update") => return Err(anyhow!("update event, already processed")),
        None => {} // Accept if no type specified (backward compat)
        Some(other) => return Err(anyhow!("unknown event type {other:?}, not sealed")),
    }

    let event = wrapper
        .after
        .ok_or_else(|| anyhow!("missing 'after' section in event"))?;

    if event.false_positive.unwrap_or(false) {
        return Err(anyhow!("false positive event"));
    }

    if is_identity_label(&event.label) {
        return Err(anyhow!("identity label refused, not sealed"));
    }

    let confidence = event.top_score.or(event.score).unwrap_or(0.0);

    // Prefer entered_zones over current_zones for more complete zone coverage
    let zones = if !event.entered_zones.is_empty() {
        event.entered_zones
    } else {
        event.current_zones
    };

    Ok(ParsedFrigateEvent {
        camera: event.camera,
        labels: vec![event.label],
        confidence,
        zones,
        kind: FrigateClaimKind::TrackedObject,
    })
}

fn is_alert(severity: Option<&str>) -> bool {
    severity == Some("alert")
}

/// Parse a Frigate review message (`frigate/reviews`, 0.14+ before/after
/// schema) into a claim, sealing an alert exactly once.
///
/// Returns `Ok` only for the message that makes a review an alert: a `new`
/// whose `after.severity` is already `alert`, or an `update` whose `after`
/// is `alert` and whose `before` was not. Detections, later updates and
/// `end` messages are errors ("not sealed"), as is a review whose objects
/// are only identity labels. A review carries no score, so the claim's
/// confidence is `1.0`.
pub fn parse_review_event(payload: &[u8]) -> Result<ParsedFrigateEvent> {
    let review: FrigateReview =
        serde_json::from_slice(payload).map_err(|e| anyhow!("parse error: {}", e))?;

    let after = review
        .after
        .ok_or_else(|| anyhow!("missing 'after' section in review"))?;
    let after_alert = is_alert(after.severity.as_deref());

    match review.review_type.as_deref() {
        Some("new") => {
            if !after_alert {
                return Err(anyhow!("review is not an alert, not sealed"));
            }
        }
        Some("update") => {
            if !after_alert {
                return Err(anyhow!("review update is not an alert, not sealed"));
            }
            let before_alert = review
                .before
                .as_ref()
                .is_some_and(|before| is_alert(before.severity.as_deref()));
            if before_alert {
                return Err(anyhow!("review was already an alert, already processed"));
            }
        }
        Some("end") => return Err(anyhow!("review ended, already processed")),
        other => return Err(anyhow!("review type {other:?} is not sealed")),
    }

    let data = after.data.ok_or_else(|| anyhow!("no review data"))?;
    let labels: Vec<String> = data
        .objects
        .into_iter()
        .filter(|object| !is_identity_label(object))
        .collect();
    if labels.is_empty() {
        return Err(anyhow!("review names no object class to seal"));
    }

    Ok(ParsedFrigateEvent {
        camera: after.camera,
        labels,
        confidence: 1.0,
        zones: data.zones,
        kind: FrigateClaimKind::ReviewAlert,
    })
}

/// Map a Frigate object label to a PWK EventType, or `None` for a label
/// that names an identity ([`IDENTITY_LABELS`]) and therefore yields no
/// event at all. Labels are base labels only (no `label:sub_label` fold).
pub fn map_label_to_event_type(label: &str) -> Option<EventType> {
    if is_identity_label(label) {
        return None;
    }
    Some(match label.to_lowercase().as_str() {
        "person" => EventType::BoundaryCrossingObjectLarge,
        "car" | "truck" | "bus" | "motorcycle" => EventType::BoundaryCrossingObjectLarge,
        "dog" | "cat" | "bird" | "animal" => EventType::BoundaryCrossingObjectSmall,
        "bicycle" | "skateboard" => EventType::BoundaryCrossingObjectSmall,
        "package" | "box" => EventType::BoundaryCrossingObjectSmall,
        _ => EventType::BoundaryCrossingObjectSmall,
    })
}

/// Choose the event type for a claim from the labels it names, honoring the
/// operator's label allowlist (`allowed` holds lowercase labels). A tracked
/// object names one label; a review alert may name several, and the claim
/// takes the coarsest class any allowed label maps to (a person and a dog
/// in one alert is a large-object crossing). `None` when no label survives
/// the allowlist and the identity refusal — then there is nothing to seal.
pub fn select_event_type(labels: &[String], allowed: &[String]) -> Option<EventType> {
    let mut chosen: Option<EventType> = None;
    for label in labels {
        let lower = label.to_lowercase();
        if !allowed.contains(&lower) {
            continue;
        }
        match map_label_to_event_type(&lower) {
            Some(EventType::BoundaryCrossingObjectLarge) => {
                return Some(EventType::BoundaryCrossingObjectLarge);
            }
            Some(event_type) => chosen = chosen.or(Some(event_type)),
            None => {}
        }
    }
    chosen
}

/// The camera-name slug shared with the add-on wizard (spec contract C8):
/// ASCII-lowercase, then every code point outside `[a-z0-9]` becomes `_`
/// (per code point, no run collapsing, no trimming), truncated to 64. The
/// wizard writes the allowlist with the same rule, and the bridge compares
/// `camera_slug(payload.camera) == camera_slug(allowlist entry)`, so
/// `Front Door`, `front-door` and `front_door` all name one camera. Zone ids
/// keep their own rule ([`sanitize_zone_name`]); the two never meet.
pub fn camera_slug(name: &str) -> String {
    name.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else {
                '_'
            }
        })
        .take(64)
        .collect()
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
        let event = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes()).expect("parse");
        assert_eq!(event.camera, "front_door");
        assert_eq!(event.labels, vec!["person"]);
        assert!((event.confidence - 0.92).abs() < 0.001);
        assert_eq!(event.zones, vec!["driveway", "porch"]);
        assert_eq!(event.kind, FrigateClaimKind::TrackedObject);
    }

    #[test]
    fn parse_update_event_rejected() {
        let payload = r#"{"after": {"camera": "x", "label": "y"}, "type": "update"}"#;
        let err = parse_frigate_event(payload.as_bytes()).unwrap_err();
        assert!(err.to_string().contains("already processed"));
    }

    #[test]
    fn sub_label_pair_does_not_fail_the_message_and_is_not_folded() {
        let payload = r#"{"after": {"camera": "x", "label": "person",
            "sub_label": ["Someone Known", 0.79], "top_score": 0.9}, "type": "new"}"#;
        let event = parse_frigate_event(payload.as_bytes()).expect("tolerant parse");
        assert_eq!(event.labels, vec!["person"]);
    }

    #[test]
    fn null_kept_collections_read_as_empty() {
        let payload = r#"{"after": {"camera": "x", "label": "car",
            "current_zones": null, "entered_zones": null, "false_positive": null,
            "score": null, "top_score": null}, "type": "new"}"#;
        let event = parse_frigate_event(payload.as_bytes()).expect("tolerant parse");
        assert!(event.zones.is_empty());
        assert_eq!(event.confidence, 0.0);
    }

    #[test]
    fn identity_labels_are_refused() {
        for label in ["face", "license_plate", "Face"] {
            let payload =
                format!(r#"{{"after": {{"camera": "x", "label": "{label}"}}, "type": "new"}}"#);
            assert!(parse_frigate_event(payload.as_bytes()).is_err(), "{label}");
            assert_eq!(map_label_to_event_type(label), None, "{label}");
        }
    }

    #[test]
    fn map_person_to_large() {
        assert_eq!(
            map_label_to_event_type("person"),
            Some(EventType::BoundaryCrossingObjectLarge)
        );
    }

    #[test]
    fn map_dog_to_small() {
        assert_eq!(
            map_label_to_event_type("dog"),
            Some(EventType::BoundaryCrossingObjectSmall)
        );
    }

    #[test]
    fn select_event_type_takes_the_coarsest_allowed_class() {
        let allowed: Vec<String> = ["person", "dog"].iter().map(|s| s.to_string()).collect();
        let labels: Vec<String> = ["dog", "person"].iter().map(|s| s.to_string()).collect();
        assert_eq!(
            select_event_type(&labels, &allowed),
            Some(EventType::BoundaryCrossingObjectLarge)
        );
        let only_dog = vec!["dog".to_string()];
        assert_eq!(
            select_event_type(&only_dog, &allowed),
            Some(EventType::BoundaryCrossingObjectSmall)
        );
        let not_allowed = vec!["bird".to_string()];
        assert_eq!(select_event_type(&not_allowed, &allowed), None);
        // An allowlisted identity label still yields nothing.
        let allowed_face = vec!["face".to_string()];
        let face = vec!["face".to_string()];
        assert_eq!(select_event_type(&face, &allowed_face), None);
    }

    #[test]
    fn subscriptions_are_one_topic_per_mode() {
        assert_eq!(
            frigate_subscriptions(FrigateMode::Events, "frigate", None),
            vec!["frigate/events"]
        );
        assert_eq!(
            frigate_subscriptions(FrigateMode::Events, "frigate", Some("nvr/events")),
            vec!["nvr/events"]
        );
        assert_eq!(
            frigate_subscriptions(FrigateMode::Reviews, "frigate", Some("nvr/events")),
            vec!["frigate/reviews"]
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

    #[test]
    fn camera_slug_follows_the_shared_rule() {
        assert_eq!(camera_slug("Front Door"), "front_door");
        assert_eq!(camera_slug("Café"), "caf_");
        assert_eq!(camera_slug(&"x".repeat(100)).len(), 64);
    }
}
