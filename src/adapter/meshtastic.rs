//! Meshtastic LoRa-mesh adapter (Detection Sensor Module nodes).
//!
//! [Meshtastic](https://meshtastic.org) nodes running the built-in **Detection Sensor Module**
//! (firmware ≥ 2.2.2) turn a GPIO input — PIR, contact switch, acoustic trigger — into a text
//! alert broadcast over the LoRa mesh. A gateway node with the MQTT module enabled
//! (`mqtt.enabled = true`, `mqtt.json_enabled = true`) uplinks every packet it hears as JSON to
//! topics like `msh/<REGION>/2/json/<CHANNEL>/!<gatewayid>`. This adapter subscribes to those
//! uplinks and turns configured nodes into witness sources, giving the kernel kilometre-scale,
//! off-grid sensor reach far beyond the household ESP-NOW mesh.
//!
//! # Privacy posture
//!
//! - **Node IDs are routing keys only.** Meshtastic node numbers are pseudonymous but *stable*;
//!   they live exclusively in the operator's local config table (same exposure class as Frigate
//!   camera names) and are matched here to select a kind/zone. A [`Claim`] structurally cannot
//!   carry them, so no stable identifier reaches the sealed log or exports.
//! - **Position packets are dropped by `type` before any payload field is read** — the envelope
//!   struct has no coordinate fields, so absolute locations are never even deserialized.
//! - **Precise time is ignored.** The packet `timestamp`/`rx_time` fields are not deserialized;
//!   the host stamps a coarse [`TimeBucket`](crate::TimeBucket) and the kernel re-coarsens.
//! - **RSSI/SNR never propagate.** `snr` may be consulted as an optional quality *floor* for
//!   dropping marginal frames; it is never logged or attached to a claim.
//! - **Alert text is matched, never copied.** An optional `detection_name` gate compares the
//!   Detection Sensor Module's configured name against the alert text; `Claim` has no text field
//!   by construction.
//!
//! # Trust
//!
//! The mesh is an unauthenticated producer: anyone holding the channel PSK (the default
//! `LongFast` key is public!) can inject frames. Operators MUST use a private channel with a
//! non-default PSK, and the usual posture applies regardless — this adapter is an *audit*
//! boundary, the kernel's gates are the security boundary, and per-bucket dedup bounds a forged
//! or replayed packet to at most one coarse event per zone per bucket.
//!
//! Routing detail: claims are routed on the packet's **`from`** field (the originating sensor
//! node), never the topic's trailing `!nodeid` — that identifies the *gateway* that uplinked it.
//!
//! Gated behind the `adapter-meshtastic` feature. See `docs/meshtastic_integration.md`.

use std::collections::HashMap;
use std::sync::mpsc::{channel, Receiver, Sender};

use anyhow::Result;
use serde::{Deserialize, Serialize};

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::{LockTolerant, SensorAdapter};
use crate::EventType;

/// A `(topic, payload)` message fed to the adapter (an MQTT topic and its JSON body).
pub type MeshMessage = (String, Vec<u8>);

static MESHTASTIC_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "meshtastic_adapter",
    // Deliberately minimal (spec §7): only what a GPIO-triggered LoRa node can plausibly assert.
    allowed_claim_kinds: &[
        ClaimKind::PresenceInRestrictedZone,
        ClaimKind::ContactStateChange,
        ClaimKind::AcousticImpulseInZone,
        ClaimKind::VehicleArrivalDeparture,
    ],
    allowed_event_types: &[
        EventType::PresenceInRestrictedZone,
        EventType::ContactStateChange,
        EventType::AcousticImpulseInZone,
        EventType::VehicleArrivalDeparture,
    ],
    requested_capabilities: &[],
};

/// The claim kinds this adapter may emit (for config validation in `adapter_host`).
pub fn allowed_kinds() -> &'static [ClaimKind] {
    MESHTASTIC_DESCRIPTOR.allowed_claim_kinds
}

/// How a raw frame was carried; selects the decode path in the pure parser. The seam for future
/// transports: a serial feeder (raw `FromRadio` protobuf frames) or a protobuf-mode MQTT decoder
/// would add variants here and a decode arm in [`frame_to_claim`] — same adapter, same node table.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MeshFrameKind {
    /// Meshtastic MQTT JSON mode (`msh/<REGION>/2/json/...`, gateway `mqtt.json_enabled = true`).
    JsonMqtt,
    // Future: SerialFromRadio (ToRadio/FromRadio framing over serial/TCP),
    //         ProtoMqtt (ServiceEnvelope protobuf uplinks on msh/<REGION>/2/e/...).
}

/// Maps a Meshtastic node to a logical zone + claim kind. The node id is a routing key ONLY; it
/// never reaches a [`Claim`].
#[derive(Clone, Debug)]
pub struct MeshNode {
    /// Canonical numeric node number (parse display forms with [`parse_node_id`]).
    pub node_num: u32,
    /// The coarse claim this node's detections assert (must be in [`allowed_kinds`]).
    pub kind: ClaimKind,
    /// Logical zone label the claim is attributed to.
    pub zone_label: String,
    /// Optional gate: require the alert text to contain the Detection Sensor Module's configured
    /// `name` (case-insensitive). Also enables mapping `text`-type frames, for setups where the
    /// detection alert rides the plain text portnum. The text is matched, never copied.
    pub detection_name: Option<String>,
    /// Optional RF quality floor: drop frames whose reported SNR is below this (or missing).
    /// The value is used only for the drop decision and is never logged.
    pub min_snr: Option<f32>,
}

impl MeshNode {
    pub fn new(node_num: u32, kind: ClaimKind, zone_label: impl Into<String>) -> Self {
        Self {
            node_num,
            kind,
            zone_label: zone_label.into(),
            detection_name: None,
            min_snr: None,
        }
    }
}

/// Parse a Meshtastic node id in any of its display forms: `!7d3a9f7f` (canonical hex),
/// `0x`-prefixed hex, bare hex with at least one letter (`7d3a9f7f`), or decimal
/// (`2100993919`). Bare all-digit strings are read as decimal — use the `!` or `0x` prefix
/// for hex ids that happen to be all digits.
pub fn parse_node_id(s: &str) -> Option<u32> {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix('!') {
        return u32::from_str_radix(hex, 16).ok();
    }
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        return u32::from_str_radix(hex, 16).ok();
    }
    if s.is_empty() {
        return None;
    }
    if s.bytes().all(|b| b.is_ascii_digit()) {
        return s.parse().ok();
    }
    u32::from_str_radix(s, 16).ok()
}

/// A node table shared with the host so it can be hot-reloaded without restarting the adapter.
pub type SharedNodes = std::sync::Arc<std::sync::Mutex<Vec<MeshNode>>>;

/// Meshtastic JSON-mode envelope. Only these fields are consumed; everything else — including
/// `timestamp`, `rssi`, `hops_away`, and any position coordinates — is intentionally not
/// deserialized so it cannot be retained.
#[derive(Debug, Default, Deserialize)]
struct MeshJsonEnvelope {
    /// Originating node number (NOT the uplinking gateway — that's the topic suffix).
    #[serde(default)]
    from: Option<u64>,
    #[serde(default, rename = "type")]
    msg_type: Option<String>,
    #[serde(default)]
    snr: Option<f32>,
    #[serde(default)]
    payload: Option<MeshJsonPayload>,
}

/// Decoded payload: only the alert text (used for matching, never copied into a claim).
#[derive(Debug, Default, Deserialize)]
struct MeshJsonPayload {
    #[serde(default)]
    text: Option<String>,
}

/// Meshtastic adapter. Construct with [`MeshtasticAdapter::new`] and feed it via the returned
/// [`Sender`] (typically from an MQTT forwarder subscribed to `msh/+/2/json/+/+`).
pub struct MeshtasticAdapter {
    rx: Receiver<MeshMessage>,
    nodes: SharedNodes,
    frame_kind: MeshFrameKind,
    sandbox: bool,
    /// Last known active/inactive state per state-broadcasting node (`node_num`), used to
    /// edge-trigger heartbeat frames — see [`json_frame_to_claim_ex`]. Lives in the parent
    /// adapter, never inside the (possibly forked) sandbox closure, so it survives across
    /// `poll()` calls regardless of whether `adapter-sandbox` is enabled.
    heartbeat_state: HashMap<u32, bool>,
}

impl MeshtasticAdapter {
    /// Build the adapter from a node table; returns the feeding [`Sender`]. Frames are decoded as
    /// [`MeshFrameKind::JsonMqtt`] (the only transport in v0).
    pub fn new(nodes: Vec<MeshNode>) -> (Self, Sender<MeshMessage>) {
        let (tx, rx) = channel();
        (
            Self {
                rx,
                nodes: std::sync::Arc::new(std::sync::Mutex::new(nodes)),
                frame_kind: MeshFrameKind::JsonMqtt,
                sandbox: false,
                heartbeat_state: HashMap::new(),
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

    /// Handle to the live node table, for hot-reload by the host.
    pub fn nodes_handle(&self) -> SharedNodes {
        std::sync::Arc::clone(&self.nodes)
    }

    /// Pure transform: map one `(topic, payload)` to a claim if it is a detection frame from a
    /// configured node.
    pub fn message_to_claim(&self, topic: &str, payload: &[u8]) -> Option<Claim> {
        frame_to_claim(&self.nodes.lock_tolerant(), self.frame_kind, topic, payload)
    }
}

/// Pure, I/O-free transform (safe to run in the sandbox). The topic is accepted for parity with
/// the other adapters and future frame kinds, but routing is on the payload's `from` field.
pub fn frame_to_claim(
    nodes: &[MeshNode],
    kind: MeshFrameKind,
    _topic: &str,
    payload: &[u8],
) -> Option<Claim> {
    match kind {
        MeshFrameKind::JsonMqtt => json_frame_to_claim(nodes, payload),
    }
}

/// [`frame_to_claim`]'s heartbeat-aware counterpart — see [`json_frame_to_claim_ex`].
fn frame_to_claim_ex(
    nodes: &[MeshNode],
    kind: MeshFrameKind,
    _topic: &str,
    payload: &[u8],
) -> (Option<Claim>, Option<(u32, bool)>) {
    match kind {
        MeshFrameKind::JsonMqtt => json_frame_to_claim_ex(nodes, payload),
    }
}

/// One frame's parse result, carried across the (possibly sandboxed) parse boundary as plain
/// data — see [`MeshtasticAdapter::poll`]. `heartbeat` never reaches a [`Claim`]; it's adapter-
/// internal bookkeeping only.
#[derive(Serialize, Deserialize)]
struct ParsedFrame {
    claim: Option<Claim>,
    heartbeat: Option<(u32, bool)>,
}

fn json_frame_to_claim(nodes: &[MeshNode], payload: &[u8]) -> Option<Claim> {
    json_frame_to_claim_ex(nodes, payload).0
}

/// Same mapping as [`json_frame_to_claim`], plus the `(node_num, active)` heartbeat identity
/// when this frame was a recognized Detection Sensor state-broadcast — `None` for anything else
/// (a genuine "detected" trigger alert, which is a one-shot event and must never be deduped the
/// way a repeated heartbeat is). The claim in the return tuple is what this single frame would
/// assert in isolation, exactly as `json_frame_to_claim` already did; callers that need
/// cross-frame edge-triggering (see [`MeshtasticAdapter::poll`]) use the heartbeat identity to
/// decide whether to actually emit it.
fn json_frame_to_claim_ex(
    nodes: &[MeshNode],
    payload: &[u8],
) -> (Option<Claim>, Option<(u32, bool)>) {
    let env: MeshJsonEnvelope = match serde_json::from_slice(payload) {
        Ok(e) => e,
        Err(_) => return (None, None),
    };
    let msg_type = match env.msg_type.as_deref() {
        Some(t) => t,
        None => return (None, None),
    };
    // Everything but detection/text — position, telemetry, nodeinfo, ... — is dropped on the
    // type tag alone; in particular, position coordinates are never examined (Invariant III).
    let text_gated = match msg_type {
        "detection" => false,
        "text" => true,
        _ => return (None, None),
    };
    let from = match env.from {
        Some(f) => f,
        None => return (None, None),
    };
    let node = match nodes.iter().find(|n| u64::from(n.node_num) == from) {
        Some(n) => n,
        None => return (None, None),
    };
    if let Some(floor) = node.min_snr {
        // NaN-safe: a missing or garbage SNR never passes a configured floor.
        if !env.snr.is_some_and(|snr| snr >= floor) {
            return (None, None);
        }
    }
    let text = env.payload.as_ref().and_then(|p| p.text.as_deref());
    // Detection Sensor state-broadcast heartbeats ("<name> state: <0|1>", sent when
    // state_broadcast_interval > 0) report the *current* pin state on the same portnum as
    // trigger alerts. Only an active state may assert a claim — an inactive heartbeat must
    // never seal an event, and it would otherwise even pass a detection_name gate (the text
    // contains the sensor name).
    let heartbeat = text
        .and_then(state_broadcast_state)
        .map(|active| (node.node_num, active));
    if let Some((_, active)) = heartbeat {
        if !active {
            return (None, heartbeat);
        }
    }
    let claim = match (&node.detection_name, text_gated) {
        // Plain text frames map only through an explicit detection_name gate; otherwise an
        // operator chatting on the channel would assert presence.
        (None, true) => None,
        (None, false) => Some(claim_for(node)),
        (Some(name), _) => match text {
            Some(t) if t.to_lowercase().contains(&name.to_lowercase()) => Some(claim_for(node)),
            _ => None,
        },
    };
    (claim, heartbeat)
}

/// Recognize a Detection Sensor state-broadcast ("<name> state: <n>", firmware
/// `DetectionSensorModule.cpp`) and return whether the reported state is active.
/// Returns `None` for anything else (e.g. a "<name> detected" trigger alert).
fn state_broadcast_state(text: &str) -> Option<bool> {
    let (_, state) = text.rsplit_once(" state: ")?;
    state.trim().parse::<i64>().ok().map(|n| n != 0)
}

fn claim_for(node: &MeshNode) -> Claim {
    // A GPIO detection is binary; the host's min_confidence floor still applies.
    Claim::new(node.kind, node.zone_label.clone(), 1.0)
}

impl SensorAdapter for MeshtasticAdapter {
    fn name(&self) -> &'static str {
        "meshtastic_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &MESHTASTIC_DESCRIPTOR
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
        // Snapshot the nodes so we don't hold the lock across the sandbox fork.
        let nodes = self.nodes.lock_tolerant().clone();
        let frame_kind = self.frame_kind;
        let parse_all = || -> Result<Vec<ParsedFrame>> {
            let mut out = Vec::new();
            for (topic, payload) in &msgs {
                let (claim, heartbeat) = frame_to_claim_ex(&nodes, frame_kind, topic, payload);
                if claim.is_some() || heartbeat.is_some() {
                    out.push(ParsedFrame { claim, heartbeat });
                }
            }
            Ok(out)
        };
        #[cfg(feature = "adapter-sandbox")]
        let parsed: Vec<ParsedFrame> = if self.sandbox {
            crate::module_runtime::sandbox::run_in_sandbox(parse_all)?
        } else {
            parse_all()?
        };
        #[cfg(not(feature = "adapter-sandbox"))]
        let parsed: Vec<ParsedFrame> = parse_all()?;

        // Edge-trigger heartbeat frames here, in the parent process, so the dedup state
        // (`heartbeat_state`) survives across polls regardless of sandboxing — a node whose
        // GPIO stays continuously active (e.g. Canary Car Mode's ignition-power heartbeat, see
        // docs/hardware/canary_car_mode.md) must assert exactly once per activation, not once
        // per `state_broadcast_interval` heartbeat, however many `poll()`/bucket boundaries it
        // crosses while parked. Non-heartbeat claims (trigger alerts) pass through unchanged.
        let mut claims = Vec::with_capacity(parsed.len());
        for frame in parsed {
            match frame.heartbeat {
                Some((node_num, active)) => {
                    let was_active = self
                        .heartbeat_state
                        .insert(node_num, active)
                        .unwrap_or(false);
                    if active && !was_active {
                        if let Some(claim) = frame.claim {
                            claims.push(claim);
                        }
                    }
                }
                None => {
                    if let Some(claim) = frame.claim {
                        claims.push(claim);
                    }
                }
            }
        }
        Ok(claims)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const TOPIC: &str = "msh/US/2/json/SecuraCV/!aabbccdd";

    fn nodes() -> Vec<MeshNode> {
        vec![MeshNode::new(
            0x7d3a9f7f,
            ClaimKind::PresenceInRestrictedZone,
            "back_gate",
        )]
    }

    /// A detection frame as the gateway uplinks it; `from` is 0x7d3a9f7f in decimal.
    fn detection_frame(text: &str) -> Vec<u8> {
        format!(
            r#"{{"channel":0,"from":2100993919,"id":123,"payload":{{"text":"{text}"}},"rssi":-60,"sender":"!aabbccdd","snr":11.5,"timestamp":1721239944,"to":4294967295,"type":"detection"}}"#
        )
        .into_bytes()
    }

    #[test]
    fn detection_from_configured_node_maps_to_claim() {
        let (adapter, _tx) = MeshtasticAdapter::new(nodes());
        let claim = adapter
            .message_to_claim(TOPIC, &detection_frame("Motion detected"))
            .expect("claim");
        assert_eq!(claim.kind, ClaimKind::PresenceInRestrictedZone);
        assert_eq!(claim.zone_label, "back_gate");
        assert_eq!(claim.confidence, 1.0);
        assert!(claim.dedup_hint.is_none());
    }

    #[test]
    fn routing_is_on_from_field_not_gateway_topic() {
        let (adapter, _tx) = MeshtasticAdapter::new(nodes());
        // Unconfigured originator drops even though the gateway topic id is fixed.
        let other =
            br#"{"from":1234,"payload":{"text":"Motion detected"},"snr":10.0,"type":"detection"}"#;
        assert!(adapter.message_to_claim(TOPIC, other).is_none());
        // Missing `from` drops.
        let no_from = br#"{"payload":{"text":"Motion detected"},"type":"detection"}"#;
        assert!(adapter.message_to_claim(TOPIC, no_from).is_none());
    }

    #[test]
    fn position_and_telemetry_frames_are_dropped_even_from_configured_nodes() {
        let (adapter, _tx) = MeshtasticAdapter::new(nodes());
        let position = br#"{"from":2100993919,"payload":{"latitude_i":407100000,"longitude_i":-740100000},"type":"position"}"#;
        assert!(adapter.message_to_claim(TOPIC, position).is_none());
        let telemetry = br#"{"from":2100993919,"payload":{"battery_level":87},"type":"telemetry"}"#;
        assert!(adapter.message_to_claim(TOPIC, telemetry).is_none());
        let nodeinfo =
            br#"{"from":2100993919,"payload":{"longname":"Back Gate PIR"},"type":"nodeinfo"}"#;
        assert!(adapter.message_to_claim(TOPIC, nodeinfo).is_none());
    }

    #[test]
    fn text_frames_map_only_through_a_detection_name_gate() {
        let mut gated = nodes();
        gated[0].detection_name = Some("PIR".to_string());
        let (adapter, _tx) = MeshtasticAdapter::new(gated);
        let text =
            br#"{"from":2100993919,"payload":{"text":"PIR detected"},"snr":9.0,"type":"text"}"#;
        let claim = adapter.message_to_claim(TOPIC, text).expect("claim");
        assert_eq!(claim.zone_label, "back_gate");
        // Non-matching text drops (someone chatting on the channel).
        let chat =
            br#"{"from":2100993919,"payload":{"text":"on my way home"},"snr":9.0,"type":"text"}"#;
        assert!(adapter.message_to_claim(TOPIC, chat).is_none());

        // Without a gate, text frames never map.
        let (ungated, _tx) = MeshtasticAdapter::new(nodes());
        assert!(ungated.message_to_claim(TOPIC, text).is_none());
    }

    #[test]
    fn detection_name_gate_applies_to_detection_frames_too() {
        let mut gated = nodes();
        gated[0].detection_name = Some("Back Gate".to_string());
        let (adapter, _tx) = MeshtasticAdapter::new(gated);
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("Back Gate detected"))
            .is_some());
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("Front Door detected"))
            .is_none());
    }

    #[test]
    fn inactive_state_broadcasts_never_assert_a_claim() {
        // state_broadcast_interval > 0 sends "<name> state: <0|1>" heartbeats on the same
        // portnum as trigger alerts; only an active state may map.
        let (adapter, _tx) = MeshtasticAdapter::new(nodes());
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("PIR state: 0"))
            .is_none());
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("PIR state: 1"))
            .is_some());

        // The inactive heartbeat must drop even through a detection_name gate — the text
        // contains the sensor name.
        let mut gated = nodes();
        gated[0].detection_name = Some("PIR".to_string());
        let (gated_adapter, _tx) = MeshtasticAdapter::new(gated);
        assert!(gated_adapter
            .message_to_claim(TOPIC, &detection_frame("PIR state: 0"))
            .is_none());
        assert!(gated_adapter
            .message_to_claim(TOPIC, &detection_frame("PIR state: 1"))
            .is_some());

        // Non-numeric "state:" text is not a recognized heartbeat — treated as trigger text.
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("strange state: open"))
            .is_some());
    }

    #[test]
    fn vehicle_arrival_departure_node_asserts_on_active_heartbeat_only() {
        // Canary Car Mode: a GPIO reading the vehicle's ignition-switched USB power, configured
        // with state_broadcast_interval > 0 so it periodically re-affirms "still running." Only
        // the active state may ever assert a claim — an inactive heartbeat (or the vehicle simply
        // going silent because it lost power) must never seal an event on its own. Departure is
        // deliberately NOT an explicit packet from the node (see
        // docs/hardware/canary_car_mode.md §4 for why) — it's inferred from the ABSENCE of these
        // heartbeats, the same pattern every other presence source in this adapter already uses.
        let nodes = vec![MeshNode::new(
            0x7d3a9f7f,
            ClaimKind::VehicleArrivalDeparture,
            "garage",
        )];
        let (adapter, _tx) = MeshtasticAdapter::new(nodes);
        let claim = adapter
            .message_to_claim(TOPIC, &detection_frame("Car Mode state: 1"))
            .expect("active heartbeat asserts");
        assert_eq!(claim.kind, ClaimKind::VehicleArrivalDeparture);
        assert_eq!(claim.zone_label, "garage");
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("Car Mode state: 0"))
            .is_none());
    }

    #[test]
    fn snr_floor_drops_marginal_and_snr_less_frames() {
        let mut floored = nodes();
        floored[0].min_snr = Some(-10.0);
        let (adapter, _tx) = MeshtasticAdapter::new(floored);
        assert!(adapter
            .message_to_claim(TOPIC, &detection_frame("Motion detected"))
            .is_some()); // snr 11.5 passes
        let weak = br#"{"from":2100993919,"payload":{"text":"Motion detected"},"snr":-18.0,"type":"detection"}"#;
        assert!(adapter.message_to_claim(TOPIC, weak).is_none());
        let no_snr =
            br#"{"from":2100993919,"payload":{"text":"Motion detected"},"type":"detection"}"#;
        assert!(adapter.message_to_claim(TOPIC, no_snr).is_none());
        // No floor configured: a missing snr passes.
        let (unfloored, _tx) = MeshtasticAdapter::new(nodes());
        assert!(unfloored.message_to_claim(TOPIC, no_snr).is_some());
    }

    #[test]
    fn malformed_input_yields_nothing() {
        let (adapter, _tx) = MeshtasticAdapter::new(nodes());
        assert!(adapter.message_to_claim(TOPIC, b"not json").is_none());
        assert!(adapter.message_to_claim(TOPIC, b"").is_none());
        assert!(adapter
            .message_to_claim(TOPIC, &[0xff, 0xfe, 0x00])
            .is_none());
        assert!(adapter
            .message_to_claim(TOPIC, br#"{"from":2100993919}"#)
            .is_none());
    }

    #[test]
    fn parse_node_id_accepts_all_display_forms() {
        assert_eq!(parse_node_id("!7d3a9f7f"), Some(0x7d3a9f7f));
        assert_eq!(parse_node_id("7d3a9f7f"), Some(0x7d3a9f7f));
        assert_eq!(parse_node_id("2100993919"), Some(0x7d3a9f7f));
        assert_eq!(parse_node_id(" !7d3a9f7f "), Some(0x7d3a9f7f));
        // All-digit strings are decimal; force hex with the `!` or `0x` prefix.
        assert_eq!(parse_node_id("12345678"), Some(12_345_678));
        assert_eq!(parse_node_id("!12345678"), Some(0x12345678));
        assert_eq!(parse_node_id("0x7d3a9f7f"), Some(0x7d3a9f7f));
        assert_eq!(parse_node_id("0X12345678"), Some(0x12345678));
        assert_eq!(parse_node_id(""), None);
        assert_eq!(parse_node_id("not-a-node"), None);
        assert_eq!(parse_node_id("!zzzz"), None);
        assert_eq!(parse_node_id("0xzzzz"), None);
    }

    #[test]
    fn poll_drains_and_dedup_is_left_to_host() {
        let (mut adapter, tx) = MeshtasticAdapter::new(nodes());
        tx.send((TOPIC.to_string(), detection_frame("Motion detected")))
            .unwrap();
        tx.send((TOPIC.to_string(), detection_frame("Motion detected")))
            .unwrap();
        tx.send((TOPIC.to_string(), b"garbage".to_vec())).unwrap();
        let claims = adapter.poll().expect("poll");
        // Two raw detections → two claims here; the host collapses them per bucket.
        assert_eq!(claims.len(), 2);
    }

    #[test]
    fn repeated_active_heartbeat_asserts_only_once_across_polls() {
        // Codex review (PR #1168): AdapterHost only dedups within the current time bucket, so a
        // Car Mode node's periodic "still running" heartbeat (state_broadcast_interval) must not
        // seal a fresh vehicle_arrival_departure claim on every poll while the vehicle is simply
        // still parked with the engine running — only the inactive->active transition may.
        let nodes = vec![MeshNode::new(
            0x7d3a9f7f,
            ClaimKind::VehicleArrivalDeparture,
            "garage",
        )];
        let (mut adapter, tx) = MeshtasticAdapter::new(nodes);

        tx.send((TOPIC.to_string(), detection_frame("Car Mode state: 1")))
            .unwrap();
        let first = adapter.poll().expect("poll");
        assert_eq!(first.len(), 1, "first active heartbeat asserts");

        // Several more active heartbeats, across separate poll() calls (i.e. separate potential
        // time-bucket boundaries) — none should re-assert while the node stays continuously on.
        for _ in 0..3 {
            tx.send((TOPIC.to_string(), detection_frame("Car Mode state: 1")))
                .unwrap();
            let claims = adapter.poll().expect("poll");
            assert!(
                claims.is_empty(),
                "repeated active heartbeat must not re-assert"
            );
        }

        // Go inactive, then active again — this IS a real transition and must assert.
        tx.send((TOPIC.to_string(), detection_frame("Car Mode state: 0")))
            .unwrap();
        let inactive = adapter.poll().expect("poll");
        assert!(inactive.is_empty(), "inactive heartbeat never asserts");

        tx.send((TOPIC.to_string(), detection_frame("Car Mode state: 1")))
            .unwrap();
        let reasserted = adapter.poll().expect("poll");
        assert_eq!(
            reasserted.len(),
            1,
            "re-activation after a real gap asserts again"
        );
    }

    #[test]
    fn repeated_trigger_alerts_are_never_deduped_like_heartbeats() {
        // A genuine "detected" trigger alert (no "state:" suffix) is a one-shot event, not a
        // heartbeat — repeated detections are meaningful and must each produce a claim, unlike
        // repeated active heartbeats above.
        let (mut adapter, tx) = MeshtasticAdapter::new(nodes());
        for _ in 0..3 {
            tx.send((TOPIC.to_string(), detection_frame("Motion detected")))
                .unwrap();
        }
        let claims = adapter.poll().expect("poll");
        assert_eq!(claims.len(), 3);
    }
}
