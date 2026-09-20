//! Turning the wire into cards.
//!
//! The surface consumes exactly the MQTT entity set a Canary already
//! announces to Home Assistant — the rule Canary Cards is built on
//! (`docs/standard/CANARY_CARDS.md` §3: "a surface renders cards for exactly
//! the entities the device announces"). This module is that projection for
//! the hub, and it is the only place a payload field becomes a card.
//!
//! The card set below mirrors `canary-local/assets/canary-cards.js`
//! `senseCards()` — same ids, same kinds, same privacy classes, same order —
//! so a Canary looks the same on this glass as on the Sense Lab bench and on
//! the wall display's card strip. Where a field is missing from the payload
//! the card is built with a `null` value, which renders as unknown, never as
//! zero (AD-Core §2.1: silence is not safety).
//!
//! # What this module does not do
//!
//! It does not verify anything. Chain verification, TOFU key pinning, replay
//! rejection and the online/offline verdict all come from
//! [`crate::fleet_peers::PeerTable`], which the daemon feeds with the same
//! messages and which already does that work for the kernel's own
//! `/api/fleet`. Writing a second verifier here would be the parallel path
//! FR-13 exists to prevent, and the second one would be the one that got the
//! replay rule wrong.

use serde_json::Value;

use super::card::{Badge, Card, CardValue, PrivacyClass};

/// Highest number of cards built for one device. The announced entity set is
/// short and fixed; this is the bound that keeps a malformed payload from
/// growing one (FR-4).
pub const MAX_CARDS_PER_DEVICE: usize = 16;

fn card(id: &str, title: &str, privacy: PrivacyClass, value: CardValue) -> Card {
    Card {
        v: super::card::CARD_SCHEMA_V,
        id: id.to_string(),
        title: title.to_string(),
        privacy: Some(privacy),
        severity: None,
        absent: false,
        value,
    }
}

fn opt_bool(doc: &Value, key: &str) -> Option<bool> {
    doc.get(key).and_then(Value::as_bool)
}

fn opt_f64(doc: &Value, key: &str) -> Option<f64> {
    doc.get(key).and_then(Value::as_f64)
}

fn opt_str(doc: &Value, key: &str) -> Option<String> {
    doc.get(key)
        .and_then(Value::as_str)
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

/// Build the card set from a retained `securacv/<device>/state` payload.
///
/// A `presence_state` of `"unknown"` yields a `null` presence card, not
/// `false`: a radar that has stopped answering reports unknown presence, and
/// a surface that rendered that as "clear" would be inventing an all-clear
/// nobody claimed.
pub fn cards_from_state(payload: &[u8]) -> Vec<Card> {
    let Ok(doc) = serde_json::from_slice::<Value>(payload) else {
        return Vec::new();
    };
    let mut out = Vec::with_capacity(MAX_CARDS_PER_DEVICE);

    let presence_state = opt_str(&doc, "presence_state");
    let presence = match presence_state.as_deref() {
        Some("present") => Some(true),
        Some("clear") => Some(false),
        // "unknown", absent, or anything else: unknown.
        _ => None,
    };
    out.push(card(
        "presence",
        "Presence",
        PrivacyClass::P0,
        CardValue::Binary(presence),
    ));

    out.push(card(
        "occupants",
        "Occupants",
        PrivacyClass::P0,
        CardValue::Band {
            options: vec!["0".into(), "1".into(), "2+".into()],
            value: if presence.is_none() {
                None
            } else {
                opt_str(&doc, "occupants")
            },
        },
    ));

    // P2: raw centimeters never leave the radar, and the band is all any
    // surface ever sees. It is carried here so the classing gate has a real
    // P2 card to refuse rather than a hypothetical one.
    out.push(card(
        "range_band",
        "Range band",
        PrivacyClass::P2,
        CardValue::Band {
            options: vec!["near".into(), "mid".into(), "far".into()],
            value: opt_str(&doc, "range").filter(|r| r != "unknown"),
        },
    ));

    out.push(card(
        "radar_link",
        "Radar link",
        PrivacyClass::P0,
        CardValue::Binary(opt_bool(&doc, "radar_ok")),
    ));

    out.push(card(
        "frame_errors",
        "Frame errors",
        PrivacyClass::P0,
        CardValue::Stat {
            value: opt_f64(&doc, "frame_errors"),
            unit: String::new(),
        },
    ));

    out.push(card(
        "illuminance",
        "Illuminance",
        PrivacyClass::P0,
        CardValue::Stat {
            value: opt_f64(&doc, "lux").filter(|v| *v >= 0.0),
            unit: "lx".into(),
        },
    ));

    out.push(card(
        "breathing",
        "Breathing confirmed",
        PrivacyClass::P0,
        CardValue::Binary(opt_bool(&doc, "breathing_locked")),
    ));

    for (id, title, key) in [
        ("breath_rate", "Breath rate", "breath_bpm"),
        ("heart_rate", "Heart rate", "heart_bpm"),
    ] {
        out.push(card(
            id,
            title,
            PrivacyClass::P1,
            CardValue::Sparkline {
                value: opt_f64(&doc, key),
                unit: "bpm".into(),
            },
        ));
    }

    out.push(card(
        "last_event",
        "Last event",
        PrivacyClass::P0,
        CardValue::Event {
            value: opt_str(&doc, "last_event"),
            signed: None,
        },
    ));

    out.truncate(MAX_CARDS_PER_DEVICE);
    out
}

/// The trust card, from the chain length and the badge the peer table
/// arrived at.
pub fn chain_card(chain_len: u64, badge: Badge) -> Card {
    card(
        "chain",
        "Witness chain",
        PrivacyClass::P0,
        CardValue::Trust {
            chain: chain_len,
            badge,
        },
    )
}

/// The tamper card from a `securacv/<device>/tamper` payload, if it reports
/// interference. `None` when the payload does not say `state: "on"` — a
/// cleared or malformed tamper message is not a claim.
pub fn tamper_card(payload: &[u8]) -> Option<Card> {
    let doc = serde_json::from_slice::<Value>(payload).ok()?;
    let on = doc.get("state").and_then(Value::as_str).map(str::trim) == Some("on");
    on.then(|| {
        card(
            "tamper",
            "Tamper reported",
            PrivacyClass::P0,
            CardValue::Binary(Some(true)),
        )
    })
}

/// The heard-alarm card from a `securacv/<device>/sensing` payload.
///
/// Gates on `acoustic_event`, the enum field that means *now* — never on the
/// cumulative `t3_detected` / `t4_detected` counters, which stay nonzero
/// forever once anything has ever been heard and would therefore re-raise the
/// advisory on every heartbeat. This is the same field and the same reasoning
/// as the alert relay's `sensing` arm and the adapter host's route gate; the
/// vocabulary decision lives in [`crate::relay::acoustic_advisory`] and this
/// function only picks which card it becomes.
pub fn acoustic_card(payload: &[u8]) -> Option<Card> {
    let advisory = crate::relay::acoustic_advisory(payload)?;
    let (id, title) = match advisory {
        crate::relay::AcousticAdvisory::Smoke => ("smoke_alarm_heard", "Smoke alarm heard"),
        crate::relay::AcousticAdvisory::CarbonMonoxide => ("co_alarm_heard", "CO alarm heard"),
    };
    Some(card(
        id,
        title,
        PrivacyClass::P0,
        CardValue::Binary(Some(true)),
    ))
}

/// Map the peer table's chain word onto a card badge.
///
/// `"ok"` means a live signed publish verified against this device's pinned
/// key, which is the one thing that earns [`Badge::Verified`] — AD-Core §2.5,
/// no overclaiming. Everything softer keeps a softer word.
pub fn badge_from_chain_word(word: &str, proven: bool) -> Badge {
    match word {
        "ok" => Badge::Verified,
        "degraded" if proven => Badge::Failed,
        "degraded" => Badge::Unsigned,
        _ => Badge::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surface::busybar::card::{CardKind, PrivacyClass};

    const STATE: &[u8] = br#"{
        "device_id":"porch","device_type":"canary-sense",
        "presence":true,"presence_state":"present","occupants":"1",
        "range":"near","radar_ok":true,"frame_errors":2,"lux":41.0,
        "breathing_locked":true,"breath_bpm":14.0,"heart_bpm":62.0,
        "last_event":"presence_in_restricted_zone","uptime_s":900,"ts_ms":1700000000000
    }"#;

    fn find<'a>(cards: &'a [Card], id: &str) -> &'a Card {
        cards.iter().find(|c| c.id == id).expect(id)
    }

    #[test]
    fn a_state_publish_becomes_the_announced_card_set() {
        let cards = cards_from_state(STATE);
        assert!(cards.len() <= MAX_CARDS_PER_DEVICE);
        assert_eq!(
            find(&cards, "presence").value,
            CardValue::Binary(Some(true))
        );
        assert_eq!(find(&cards, "presence").privacy, Some(PrivacyClass::P0));
        assert_eq!(find(&cards, "range_band").privacy, Some(PrivacyClass::P2));
        assert_eq!(find(&cards, "heart_rate").privacy, Some(PrivacyClass::P1));
        assert_eq!(find(&cards, "illuminance").kind(), CardKind::Stat);
        assert_eq!(find(&cards, "breath_rate").kind(), CardKind::Sparkline);
    }

    /// A radar that has stopped answering reports unknown presence. Rendering
    /// that as "clear" would be inventing an all-clear nobody claimed
    /// (AD-Core section 2.1).
    #[test]
    fn unknown_presence_stays_unknown_and_takes_the_count_with_it() {
        let cards = cards_from_state(br#"{"presence_state":"unknown","occupants":"2+"}"#);
        assert_eq!(find(&cards, "presence").value, CardValue::Binary(None));
        match &find(&cards, "occupants").value {
            CardValue::Band { value, .. } => assert_eq!(*value, None),
            other => panic!("occupants should be a band: {other:?}"),
        }
    }

    #[test]
    fn a_clear_reading_is_not_the_same_as_no_reading() {
        let clear = cards_from_state(br#"{"presence_state":"clear"}"#);
        assert_eq!(
            find(&clear, "presence").value,
            CardValue::Binary(Some(false))
        );
    }

    #[test]
    fn a_malformed_payload_yields_no_cards_rather_than_wrong_ones() {
        assert!(cards_from_state(b"not json").is_empty());
        assert!(cards_from_state(b"").is_empty());
    }

    #[test]
    fn a_missing_field_reads_unknown_never_zero() {
        let cards = cards_from_state(br#"{"presence_state":"present"}"#);
        match &find(&cards, "illuminance").value {
            CardValue::Stat { value, .. } => assert_eq!(*value, None),
            other => panic!("illuminance should be a stat: {other:?}"),
        }
        assert_eq!(find(&cards, "radar_link").value, CardValue::Binary(None));
    }

    #[test]
    fn only_a_live_tamper_report_is_a_claim() {
        assert!(tamper_card(br#"{"state":"on","confidence":0.93}"#).is_some());
        assert!(tamper_card(br#"{"state":"off"}"#).is_none());
        assert!(tamper_card(b"{}").is_none());
        assert!(tamper_card(b"garbage").is_none());
    }

    /// The counter trap, pinned. `t3_detected` stays nonzero forever once
    /// anything has ever been heard; gating on it would re-raise the advisory
    /// on every 60-second heartbeat.
    #[test]
    fn the_advisory_gates_on_the_enum_and_never_on_the_counters() {
        let now = br#"{"acoustic_event":"smoke_alarm_t3","t3_detected":5,"t4_detected":0}"#;
        assert_eq!(
            acoustic_card(now).map(|c| c.id),
            Some("smoke_alarm_heard".into())
        );

        let heartbeat = br#"{"acoustic_event":"none","t3_detected":5,"t4_detected":2}"#;
        assert!(
            acoustic_card(heartbeat).is_none(),
            "a heartbeat carrying old counters must not raise an advisory"
        );

        let co = br#"{"acoustic_event":"co_alarm_t4","t4_detected":1}"#;
        assert_eq!(
            acoustic_card(co).map(|c| c.id),
            Some("co_alarm_heard".into())
        );
    }

    /// "Verified" is earned, not assumed (AD-Core section 2.5).
    #[test]
    fn only_a_checked_signature_earns_the_word_verified() {
        assert_eq!(badge_from_chain_word("ok", true), Badge::Verified);
        assert_eq!(badge_from_chain_word("degraded", true), Badge::Failed);
        assert_eq!(badge_from_chain_word("degraded", false), Badge::Unsigned);
        assert_eq!(badge_from_chain_word("unknown", false), Badge::Unknown);
        assert_eq!(badge_from_chain_word("", false), Badge::Unknown);
    }

    /// Parity with the reference renderer. `canary-local/assets/canary-cards.js`
    /// `senseCards()` is the schema's reference implementation and the
    /// firmware's card model is already pinned to it
    /// (`tests_host/test_fleet_cards.cpp`). This is the third surface, so it
    /// is pinned the same way: every card the Lab bench builds for a Canary
    /// Sense exists here, under the same id, in the same privacy class.
    ///
    /// A drift here means a household sees one classing on the Sense Lab and
    /// a different one on the desk — which, since the classing is what
    /// decides who may read a card, is a privacy bug rather than a cosmetic
    /// one.
    #[test]
    fn the_card_set_matches_the_reference_renderer() {
        let js_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("canary-local/assets/canary-cards.js");
        let js = std::fs::read_to_string(&js_path)
            .unwrap_or_else(|e| panic!("reading {}: {e}", js_path.display()));
        let body = js
            .split_once("export function senseCards(")
            .expect("senseCards() in the reference renderer")
            .1;

        // Each descriptor literal is `id: "...", ... privacy: "P0"`. Walk the
        // ids in order and read the privacy class that follows each one
        // before the next id begins.
        let mut expected: Vec<(String, String)> = Vec::new();
        let chunks: Vec<&str> = body.split("id: \"").skip(1).collect();
        for chunk in chunks {
            let (id, rest) = chunk.split_once('"').expect("id literal");
            let upto = rest.split("id: \"").next().unwrap_or(rest);
            let privacy = upto
                .split_once("privacy: \"")
                .map(|(_, p)| p.split_once('"').expect("privacy literal").0.to_string())
                .unwrap_or_default();
            expected.push((id.to_string(), privacy));
        }
        assert!(
            expected.len() >= 10,
            "parsed only {} cards from the reference renderer",
            expected.len()
        );

        let mut ours: Vec<Card> = cards_from_state(STATE);
        ours.push(chain_card(12, Badge::Verified));

        for (id, privacy) in &expected {
            let card = ours.iter().find(|c| &c.id == id).unwrap_or_else(|| {
                panic!("the reference renderer builds `{id}` and this does not")
            });
            let want = match privacy.as_str() {
                "P0" => Some(PrivacyClass::P0),
                "P1" => Some(PrivacyClass::P1),
                "P2" => Some(PrivacyClass::P2),
                other => panic!("unexpected privacy class {other:?} for `{id}`"),
            };
            assert_eq!(
                card.privacy, want,
                "`{id}` is {:?} here and {privacy} in the reference renderer",
                card.privacy
            );
        }
    }
}
