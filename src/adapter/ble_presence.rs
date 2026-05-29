//! BLE presence-gateway adapter (ESPresense / room-presence style).
//!
//! Popular DIY BLE presence systems — most notably [ESPresense](https://espresense.com) — place
//! cheap ESP32 nodes in each room and publish per-room distance estimates over MQTT, e.g. to
//! `espresense/devices/<device-id>/<room>` with a JSON body like `{"distance":2.3,"rssi":-70}`.
//!
//! This adapter turns that ecosystem into a witness source **without importing any identity**: it
//! deliberately ignores the device id/name entirely and emits only a coarse
//! `presence_in_restricted_zone` claim for the *room* when the reported distance is within a
//! configured threshold. The room (the last topic segment) maps to a logical zone; the per-bucket
//! dedup in the host collapses ESPresense's high-frequency updates into one event per bucket.
//!
//! Gated behind the `adapter-ble-presence` feature.

use std::sync::mpsc::{channel, Receiver, Sender};

use anyhow::Result;
use serde::Deserialize;

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::SensorAdapter;
use crate::EventType;

/// A `(topic, payload)` message fed to the adapter (an MQTT topic and its JSON body).
pub type BleMessage = (String, Vec<u8>);

static BLE_PRESENCE_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "ble_presence_adapter",
    allowed_claim_kinds: &[ClaimKind::PresenceInRestrictedZone],
    allowed_event_types: &[EventType::PresenceInRestrictedZone],
    requested_capabilities: &[],
};

/// Maps a BLE presence room to a logical zone and a presence threshold.
#[derive(Clone, Debug)]
pub struct BleRoom {
    /// Room name, matched (case-insensitively) against the last MQTT topic segment.
    pub room: String,
    /// Logical zone label the claim is attributed to.
    pub zone_label: String,
    /// Presence is asserted when the reported `distance` is at or below this (metres).
    pub max_distance: f64,
}

impl BleRoom {
    pub fn new(room: impl Into<String>, zone_label: impl Into<String>, max_distance: f64) -> Self {
        Self {
            room: room.into(),
            zone_label: zone_label.into(),
            max_distance,
        }
    }
}

/// ESPresense-style payload. Only `distance` is consumed; every other field (including any device
/// identifier) is intentionally ignored so no identity is retained.
#[derive(Debug, Default, Deserialize)]
struct BlePayload {
    #[serde(default)]
    distance: Option<f64>,
}

/// BLE presence adapter. Construct with [`BlePresenceAdapter::new`] and feed it via the returned
/// [`Sender`] (typically from an MQTT forwarder subscribed to `espresense/devices/#`).
/// A room table shared with the host so it can be hot-reloaded without restarting the adapter.
pub type SharedRooms = std::sync::Arc<std::sync::Mutex<Vec<BleRoom>>>;

pub struct BlePresenceAdapter {
    rx: Receiver<BleMessage>,
    rooms: SharedRooms,
    sandbox: bool,
}

impl BlePresenceAdapter {
    /// Build the adapter from a room table; returns the feeding [`Sender`].
    pub fn new(rooms: Vec<BleRoom>) -> (Self, Sender<BleMessage>) {
        let (tx, rx) = channel();
        (
            Self {
                rx,
                rooms: std::sync::Arc::new(std::sync::Mutex::new(rooms)),
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

    /// Handle to the live room table, for hot-reload by the host.
    pub fn rooms_handle(&self) -> SharedRooms {
        std::sync::Arc::clone(&self.rooms)
    }

    /// Pure transform: map one `(topic, payload)` to a presence claim if the room is configured
    /// and the reported distance is within threshold.
    pub fn message_to_claim(&self, topic: &str, payload: &[u8]) -> Option<Claim> {
        room_to_claim(&self.rooms.lock().expect("rooms mutex"), topic, payload)
    }
}

/// Pure, I/O-free transform (safe to run in the sandbox).
fn room_to_claim(rooms: &[BleRoom], topic: &str, payload: &[u8]) -> Option<Claim> {
    let room_segment = topic.rsplit('/').next().unwrap_or(topic);
    let room = rooms
        .iter()
        .find(|r| r.room.eq_ignore_ascii_case(room_segment))?;
    let parsed: BlePayload = serde_json::from_slice(payload).ok()?;
    let distance = parsed.distance?;
    // NaN-safe: `NaN <= x` is false, so a garbage distance never asserts presence.
    if distance <= room.max_distance {
        Some(Claim::new(
            ClaimKind::PresenceInRestrictedZone,
            room.zone_label.clone(),
            1.0,
        ))
    } else {
        None
    }
}

impl SensorAdapter for BlePresenceAdapter {
    fn name(&self) -> &'static str {
        "ble_presence_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &BLE_PRESENCE_DESCRIPTOR
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
        // Snapshot the rooms so we don't hold the lock across the sandbox fork.
        let rooms = self.rooms.lock().expect("rooms mutex").clone();
        let parse_all = || {
            let mut out = Vec::new();
            for (topic, payload) in &msgs {
                if let Some(claim) = room_to_claim(&rooms, topic, payload) {
                    out.push(claim);
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

    fn rooms() -> Vec<BleRoom> {
        vec![BleRoom::new("lobby", "lobby", 4.0)]
    }

    #[test]
    fn within_threshold_asserts_presence() {
        let (adapter, _tx) = BlePresenceAdapter::new(rooms());
        let claim = adapter
            .message_to_claim("espresense/devices/aabbcc/lobby", br#"{"distance":2.3}"#)
            .expect("claim");
        assert_eq!(claim.kind, ClaimKind::PresenceInRestrictedZone);
        assert_eq!(claim.zone_label, "lobby");
    }

    #[test]
    fn beyond_threshold_yields_nothing() {
        let (adapter, _tx) = BlePresenceAdapter::new(rooms());
        assert!(adapter
            .message_to_claim("espresense/devices/aabbcc/lobby", br#"{"distance":9.0}"#)
            .is_none());
    }

    #[test]
    fn unknown_room_yields_nothing() {
        let (adapter, _tx) = BlePresenceAdapter::new(rooms());
        assert!(adapter
            .message_to_claim("espresense/devices/aabbcc/garage", br#"{"distance":1.0}"#)
            .is_none());
    }

    #[test]
    fn missing_or_garbage_distance_yields_nothing() {
        let (adapter, _tx) = BlePresenceAdapter::new(rooms());
        assert!(adapter
            .message_to_claim("espresense/devices/aabbcc/lobby", br#"{"rssi":-70}"#)
            .is_none());
        assert!(adapter
            .message_to_claim("espresense/devices/aabbcc/lobby", b"not json")
            .is_none());
    }

    #[test]
    fn poll_drains_and_dedup_is_left_to_host() {
        let (mut adapter, tx) = BlePresenceAdapter::new(rooms());
        tx.send((
            "espresense/devices/aabbcc/lobby".to_string(),
            br#"{"distance":1.0}"#.to_vec(),
        ))
        .unwrap();
        tx.send((
            "espresense/devices/ddeeff/lobby".to_string(),
            br#"{"distance":1.5}"#.to_vec(),
        ))
        .unwrap();
        let claims = adapter.poll().expect("poll");
        // Two raw messages → two claims here; the host collapses them per bucket.
        assert_eq!(claims.len(), 2);
    }
}
