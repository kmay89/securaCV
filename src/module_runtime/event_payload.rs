use anyhow::{anyhow, Result};
use serde_json::Value;

use crate::{CandidateEvent, EventType, TimeBucket};

const ROOT_FIELDS: [&str; 4] = ["event_type", "time_bucket", "zone_id", "confidence"];
const BUCKET_FIELDS: [&str; 2] = ["start_epoch_s", "size_s"];

fn ensure_allowed_fields(
    context: &str,
    obj: &serde_json::Map<String, Value>,
    allowed: &[&str],
) -> Result<()> {
    let extras: Vec<String> = obj
        .keys()
        .filter(|key| !allowed.contains(&key.as_str()))
        .cloned()
        .collect();
    if extras.is_empty() {
        return Ok(());
    }
    Err(anyhow!(
        "conformance: {} payload contains extra fields: {}",
        context,
        extras.join(", ")
    ))
}

fn parse_event_type(value: &Value) -> Result<EventType> {
    let raw = value
        .as_str()
        .ok_or_else(|| anyhow!("conformance: event_type must be a string"))?;
    let normalized = raw.trim().to_lowercase();
    match normalized.as_str() {
        "boundary_crossing_object_large" | "boundarycrossingobjectlarge" => {
            Ok(EventType::BoundaryCrossingObjectLarge)
        }
        "boundary_crossing_object_small" | "boundarycrossingobjectsmall" => {
            Ok(EventType::BoundaryCrossingObjectSmall)
        }
        _ => Err(anyhow!("conformance: event_type not in allowed vocabulary")),
    }
}

fn parse_time_bucket(value: &Value) -> Result<TimeBucket> {
    let obj = value
        .as_object()
        .ok_or_else(|| anyhow!("conformance: time_bucket must be an object"))?;
    ensure_allowed_fields("time_bucket", obj, &BUCKET_FIELDS)?;

    let start_epoch_s = obj
        .get("start_epoch_s")
        .and_then(Value::as_u64)
        .ok_or_else(|| anyhow!("conformance: time_bucket.start_epoch_s must be u64"))?;
    let size_s = obj
        .get("size_s")
        .and_then(Value::as_u64)
        .ok_or_else(|| anyhow!("conformance: time_bucket.size_s must be u64"))?;
    let size_s = u32::try_from(size_s)
        .map_err(|_| anyhow!("conformance: time_bucket.size_s out of range"))?;

    Ok(TimeBucket {
        start_epoch_s,
        size_s,
    })
}

/// Parse an event-only payload while rejecting any extra fields.
pub fn parse_event_payload(payload: &Value) -> Result<CandidateEvent> {
    let obj = payload
        .as_object()
        .ok_or_else(|| anyhow!("conformance: payload must be a JSON object"))?;
    ensure_allowed_fields("grove_vision2", obj, &ROOT_FIELDS)?;

    let event_type = obj
        .get("event_type")
        .ok_or_else(|| anyhow!("conformance: event_type is required"))
        .and_then(parse_event_type)?;
    let time_bucket = obj
        .get("time_bucket")
        .ok_or_else(|| anyhow!("conformance: time_bucket is required"))
        .and_then(parse_time_bucket)?;
    let zone_id = obj
        .get("zone_id")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("conformance: zone_id must be a string"))?
        .to_string();
    let confidence =
        obj.get("confidence")
            .and_then(Value::as_f64)
            .ok_or_else(|| anyhow!("conformance: confidence must be a number"))? as f32;

    Ok(CandidateEvent {
        event_type,
        time_bucket,
        zone_id,
        confidence,
        correlation_token: None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parse_event_payload_accepts_minimal_payload() {
        let payload = json!({
            "event_type": "boundary_crossing_object_large",
            "time_bucket": { "start_epoch_s": 1_700_000_000u64, "size_s": 600u64 },
            "zone_id": "zone:front_gate",
            "confidence": 0.7
        });

        let cand = parse_event_payload(&payload).expect("payload should parse");
        assert_eq!(cand.event_type, EventType::BoundaryCrossingObjectLarge);
        assert_eq!(cand.time_bucket.size_s, 600);
        assert_eq!(cand.zone_id, "zone:front_gate");
    }

    #[test]
    fn parse_event_payload_rejects_extra_fields() {
        let payload = json!({
            "event_type": "boundary_crossing_object_small",
            "time_bucket": { "start_epoch_s": 1_700_000_000u64, "size_s": 600u64 },
            "zone_id": "zone:back_gate",
            "confidence": 0.4,
            "snapshot": "nope"
        });

        let err = parse_event_payload(&payload).unwrap_err();
        assert!(format!("{err}").contains("extra fields"));
    }

    #[test]
    fn parse_event_payload_rejects_extra_bucket_fields() {
        let payload = json!({
            "event_type": "boundary_crossing_object_small",
            "time_bucket": {
                "start_epoch_s": 1_700_000_000u64,
                "size_s": 600u64,
                "precise_ts": 1_700_000_001u64
            },
            "zone_id": "zone:back_gate",
            "confidence": 0.4
        });

        let err = parse_event_payload(&payload).unwrap_err();
        assert!(format!("{err}").contains("extra fields"));
    }
}
