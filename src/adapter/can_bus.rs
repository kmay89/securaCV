//! Vehicle CAN bus adapter — passive-only, arrival/departure claims.
//!
//! This is the adapter behind Canary Vehicle
//! (`docs/hardware/canary_vehicle_can.md`): a **read-only** listener on a
//! vehicle's own CAN bus that turns a configured frame-ID/byte pattern (e.g.
//! "ignition on") into a coarse [`ClaimKind::VehicleArrivalDeparture`] claim.
//!
//! # Passive-only, deliberately
//!
//! This adapter never transmits a CAN frame — it only reads. There is no
//! write path anywhere in this file, on purpose: unlike an OBD-II scan tool
//! that polls the bus with Mode 01 requests, a passive listener never risks
//! writing a malformed frame onto a customer's vehicle bus. See
//! `docs/hardware/canary_vehicle_can.md` §3 for the full reasoning.
//!
//! # Why this stays a config-only, generic adapter
//!
//! CAN frame IDs and byte layouts are manufacturer-proprietary — there is no
//! universal "ignition on" frame across vehicles. Rather than hard-code any
//! vehicle's layout, this adapter is a routing table exactly like
//! [`crate::adapter::mqtt_sensor`]: `(can_id, byte_offset, mask, value) ->
//! Claim`, populated by the operator after sniffing their *own* vehicle's bus
//! (`candump`, or any SocketCAN tool). One route per known transition (e.g.
//! one route for the ignition-on pattern, a second for ignition-off on the
//! same ID) — see the worked example in `adapter_host.example.toml`.
//!
//! # Decoupling from I/O
//!
//! Like [`crate::adapter::frigate::FrigateAdapter`], this adapter holds only
//! the receiving end of a channel; a feeder (the `adapter_host` binary's
//! SocketCAN reader thread, a serial MCP2515 bridge, or a test) pushes
//! [`CanFrame`]s into the [`Sender`] returned at construction. [`poll`]
//! drains whatever has arrived and matches it against the routing table — no
//! socket, no serial port, no I/O of its own.

use std::collections::HashMap;
use std::sync::mpsc::{channel, Receiver, Sender};

use anyhow::Result;

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::{LockTolerant, SensorAdapter};
use crate::EventType;

static CAN_BUS_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "can_bus_adapter",
    // Deliberately the narrowest possible allowlist: this adapter claims
    // arrival/departure and nothing else. It structurally cannot emit a
    // vehicle-telemetry or tracking-shaped event even if a route were
    // misconfigured, because no other ClaimKind is in this list.
    allowed_claim_kinds: &[ClaimKind::VehicleArrivalDeparture],
    allowed_event_types: &[EventType::VehicleArrivalDeparture],
    requested_capabilities: &[],
};

/// A raw CAN frame: 11-bit or 29-bit arbitration ID (stored widened to
/// `u32`) plus up to 8 data bytes. No timestamp, no bus name — the host
/// stamps a coarse `TimeBucket` exactly as it does for every other adapter.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CanFrame {
    pub can_id: u32,
    pub data: Vec<u8>,
}

/// One routing rule: a specific byte in a specific CAN ID's payload,
/// masked and compared, becomes a claim in a zone.
#[derive(Clone, Debug)]
pub struct CanRoute {
    /// Arbitration ID to match (11-bit or 29-bit — caller's responsibility
    /// to avoid collisions between the two ID spaces if both are in play).
    pub can_id: u32,
    /// Byte index into the frame's data payload to inspect (0-based).
    pub byte_offset: usize,
    /// Bitmask applied to the byte before comparison. `0xFF` compares the
    /// whole byte; a narrower mask isolates a single flag bit.
    pub mask: u8,
    /// Masked value that triggers this route's claim.
    pub equals: u8,
    /// Claim kind to emit — narrowed by [`CAN_BUS_DESCRIPTOR`] to
    /// [`ClaimKind::VehicleArrivalDeparture`] today.
    pub kind: ClaimKind,
    /// Raw zone label (e.g. "Garage"); the host sanitizes/prefixes it.
    pub zone_label: String,
    /// Confidence stamped on the claim. CAN frames carry no natural score,
    /// so this is a fixed per-route constant, not a measurement.
    pub confidence: f32,
}

impl CanRoute {
    /// Construct a route with `mask = 0xFF` (whole-byte compare) and
    /// `confidence = 0.9`.
    pub fn new(
        can_id: u32,
        byte_offset: usize,
        equals: u8,
        kind: ClaimKind,
        zone_label: impl Into<String>,
    ) -> Self {
        Self {
            can_id,
            byte_offset,
            mask: 0xFF,
            equals,
            kind,
            zone_label: zone_label.into(),
            confidence: 0.9,
        }
    }

    /// Override the default whole-byte mask (e.g. to isolate one flag bit).
    pub fn with_mask(mut self, mask: u8) -> Self {
        self.mask = mask;
        self
    }

    /// Override the default confidence.
    pub fn with_confidence(mut self, confidence: f32) -> Self {
        self.confidence = confidence;
        self
    }
}

/// Shared, hot-reloadable route table.
pub type SharedCanRoutes = std::sync::Arc<std::sync::Mutex<Vec<CanRoute>>>;

/// Stable identity of a route's condition — `(can_id, byte_offset, mask, equals)` — used as the
/// edge-detection state key instead of a vector index, so tracked state survives a SIGHUP route
/// reload as long as the route itself is unchanged.
type RouteKey = (u32, usize, u8, u8);

fn route_key(route: &CanRoute) -> RouteKey {
    (route.can_id, route.byte_offset, route.mask, route.equals)
}

fn route_matches(route: &CanRoute, data: &[u8]) -> bool {
    data.get(route.byte_offset)
        .is_some_and(|&byte| (byte & route.mask) == route.equals)
}

/// Pure, I/O-free transform: match one frame against the routing table, **level-triggered** (a
/// frame already sitting in a matched state matches every time, with no memory of prior frames).
/// This is what you want for a one-shot check; [`CanBusAdapter::poll`] wraps this in edge
/// detection (below) so a continuously-broadcast status frame doesn't mint a fresh claim every
/// poll cycle.
///
/// Multiple routes MAY share a `can_id` (e.g. one route for "ignition on", a second for
/// "ignition off" on the same frame, distinguished by `byte_offset`/`equals`) — every
/// matching-ID route is checked in order and the first whose byte condition passes wins. A frame
/// that matches no route's byte condition (or is shorter than `byte_offset`) yields no claim,
/// silently — an unrecognized frame is normal bus traffic, not an error.
pub fn route_frame(routes: &[CanRoute], can_id: u32, data: &[u8]) -> Option<Claim> {
    routes
        .iter()
        .filter(|r| r.can_id == can_id)
        .find(|r| route_matches(r, data))
        .map(|r| Claim::new(r.kind, r.zone_label.clone(), r.confidence))
}

/// Edge-triggered wrapper around [`route_frame`]'s matching logic: emits a claim only on the
/// transition INTO a route's matched state, never while a frame already sitting in that state
/// keeps arriving. Vehicles broadcast status frames continuously (often multiple times a
/// second) — without this, a parked-and-running vehicle would mint a fresh arrival claim every
/// bucket, and a naive "emit on every match" would make the "ignition off" sibling of an
/// "ignition on" route on the same `can_id` impossible to trust as a discrete event (both would
/// fire on every frame that happens to carry either byte value).
///
/// Every route sharing `can_id` has its tracked state updated on every call — not just the one
/// that ends up emitting — so the next differing frame correctly detects the next transition.
/// `last_state` is keyed by [`route_key`], not position, so it stays correct across a live route
/// reload (SIGHUP).
fn route_frame_edge_triggered(
    routes: &[CanRoute],
    last_state: &mut HashMap<RouteKey, bool>,
    can_id: u32,
    data: &[u8],
) -> Option<Claim> {
    let mut claim = None;
    for route in routes.iter().filter(|r| r.can_id == can_id) {
        let key = route_key(route);
        let matches = route_matches(route, data);
        let was_matched = last_state.insert(key, matches).unwrap_or(false);
        if matches && !was_matched && claim.is_none() {
            claim = Some(Claim::new(
                route.kind,
                route.zone_label.clone(),
                route.confidence,
            ));
        }
    }
    claim
}

/// Vehicle CAN bus adapter. Construct with [`CanBusAdapter::new`] and feed
/// it via the returned [`Sender`].
///
/// Frame matching here is trivial fixed-width byte comparison on data the kernel's own CAN
/// driver has already framed — unlike the JSON/MQTT-payload adapters, there's no meaningful
/// parser attack surface to sandbox, and the seccomp sandbox's fork-per-call model would silently
/// discard the edge-detection state this adapter needs to carry between polls. So, deliberately,
/// no `with_sandbox` here.
pub struct CanBusAdapter {
    rx: Receiver<CanFrame>,
    routes: SharedCanRoutes,
    last_state: HashMap<RouteKey, bool>,
}

impl CanBusAdapter {
    pub fn new(routes: Vec<CanRoute>) -> (Self, Sender<CanFrame>) {
        let (tx, rx) = channel();
        (
            Self {
                rx,
                routes: std::sync::Arc::new(std::sync::Mutex::new(routes)),
                last_state: HashMap::new(),
            },
            tx,
        )
    }

    /// Handle to the live route table, for SIGHUP hot-reload by the host.
    pub fn routes_handle(&self) -> SharedCanRoutes {
        std::sync::Arc::clone(&self.routes)
    }
}

impl SensorAdapter for CanBusAdapter {
    fn name(&self) -> &'static str {
        "can_bus_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &CAN_BUS_DESCRIPTOR
    }

    fn poll(&mut self) -> Result<Vec<Claim>> {
        let mut frames = Vec::new();
        while let Ok(frame) = self.rx.try_recv() {
            frames.push(frame);
        }
        if frames.is_empty() {
            return Ok(Vec::new());
        }
        let routes = self.routes.lock_tolerant().clone();
        let mut out = Vec::new();
        for frame in &frames {
            if let Some(claim) =
                route_frame_edge_triggered(&routes, &mut self.last_state, frame.can_id, &frame.data)
            {
                out.push(claim);
            }
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ignition_routes() -> Vec<CanRoute> {
        vec![
            CanRoute::new(0x3E8, 0, 0x01, ClaimKind::VehicleArrivalDeparture, "garage"),
            CanRoute::new(0x3E8, 0, 0x00, ClaimKind::VehicleArrivalDeparture, "garage"),
        ]
    }

    #[test]
    fn ignition_on_byte_pattern_becomes_a_claim() {
        let claim = route_frame(&ignition_routes(), 0x3E8, &[0x01, 0, 0, 0]).expect("claim");
        assert_eq!(claim.kind, ClaimKind::VehicleArrivalDeparture);
        assert_eq!(claim.zone_label, "garage");
    }

    #[test]
    fn ignition_off_pattern_on_the_same_id_also_becomes_a_claim() {
        let claim = route_frame(&ignition_routes(), 0x3E8, &[0x00, 0, 0, 0]).expect("claim");
        assert_eq!(claim.kind, ClaimKind::VehicleArrivalDeparture);
    }

    #[test]
    fn unmatched_byte_value_yields_no_claim() {
        assert!(route_frame(&ignition_routes(), 0x3E8, &[0x42, 0, 0, 0]).is_none());
    }

    #[test]
    fn unmatched_can_id_yields_no_claim() {
        assert!(route_frame(&ignition_routes(), 0x111, &[0x01, 0, 0, 0]).is_none());
    }

    #[test]
    fn short_frame_below_byte_offset_yields_no_claim_not_a_panic() {
        let routes = vec![CanRoute::new(
            0x3E8,
            5,
            0x01,
            ClaimKind::VehicleArrivalDeparture,
            "garage",
        )];
        assert!(route_frame(&routes, 0x3E8, &[0x01]).is_none());
    }

    #[test]
    fn mask_isolates_a_single_flag_bit() {
        // Only bit 0 matters; bit 3 (0x08) being set must not block the match.
        let routes =
            vec![
                CanRoute::new(0x3E8, 0, 0x01, ClaimKind::VehicleArrivalDeparture, "garage")
                    .with_mask(0x01),
            ];
        assert!(route_frame(&routes, 0x3E8, &[0x09]).is_some());
        assert!(route_frame(&routes, 0x3E8, &[0x08]).is_none());
    }

    #[test]
    fn poll_drains_fed_frames() {
        let (mut adapter, tx) = CanBusAdapter::new(ignition_routes());
        tx.send(CanFrame {
            can_id: 0x3E8,
            data: vec![0x01, 0, 0, 0],
        })
        .unwrap();
        let claims = adapter.poll().expect("poll");
        assert_eq!(claims.len(), 1);
        assert!(adapter.poll().expect("poll2").is_empty());
    }

    #[test]
    fn repeated_status_broadcast_fires_only_once_not_every_poll() {
        // A vehicle left running broadcasts its ignition-on frame continuously — this must NOT
        // mint a fresh arrival claim on every poll cycle (the bug the Codex review caught).
        let (mut adapter, tx) = CanBusAdapter::new(ignition_routes());
        for _ in 0..5 {
            tx.send(CanFrame {
                can_id: 0x3E8,
                data: vec![0x01, 0, 0, 0],
            })
            .unwrap();
        }
        let claims = adapter.poll().expect("poll");
        assert_eq!(
            claims.len(),
            1,
            "5 identical frames in one poll -> exactly 1 claim"
        );

        tx.send(CanFrame {
            can_id: 0x3E8,
            data: vec![0x01, 0, 0, 0],
        })
        .unwrap();
        assert!(
            adapter.poll().expect("poll2").is_empty(),
            "still-on frame on a later poll must not re-fire"
        );
    }

    #[test]
    fn ignition_on_then_off_then_on_again_fires_three_times() {
        let (mut adapter, tx) = CanBusAdapter::new(ignition_routes());
        for byte in [0x01u8, 0x00, 0x01] {
            tx.send(CanFrame {
                can_id: 0x3E8,
                data: vec![byte, 0, 0, 0],
            })
            .unwrap();
            assert_eq!(adapter.poll().expect("poll").len(), 1, "byte {byte:#x}");
        }
    }

    #[test]
    fn edge_triggered_state_survives_a_route_table_reload() {
        let (mut adapter, tx) = CanBusAdapter::new(ignition_routes());
        tx.send(CanFrame {
            can_id: 0x3E8,
            data: vec![0x01, 0, 0, 0],
        })
        .unwrap();
        assert_eq!(adapter.poll().expect("poll").len(), 1);

        // Simulate a SIGHUP reload that rebuilds an identical route list (same keys, new Vec).
        *adapter.routes_handle().lock().unwrap() = ignition_routes();

        tx.send(CanFrame {
            can_id: 0x3E8,
            data: vec![0x01, 0, 0, 0],
        })
        .unwrap();
        assert!(
            adapter.poll().expect("poll2").is_empty(),
            "still-on after a reload of the SAME route set must not re-fire"
        );
    }

    #[test]
    fn descriptor_allows_only_vehicle_arrival_departure() {
        let (adapter, _tx) = CanBusAdapter::new(vec![]);
        assert_eq!(
            adapter.descriptor().allowed_claim_kinds,
            &[ClaimKind::VehicleArrivalDeparture]
        );
    }
}
