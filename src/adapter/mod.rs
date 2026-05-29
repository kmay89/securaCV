//! Open, vendor-neutral **Sensor Adapter** framework.
//!
//! This module generalizes the one-off `frigate_bridge` pattern (parse an external source →
//! strip identity → coarsen → map to a kernel event → seal) into a small, reusable contract so
//! *any* source — acoustic/impulse sensors, PIR/contact switches, drones reduced to presence,
//! ALPR cameras down-reduced to non-identifying presence, BLE gateways, other NVRs, generic
//! MQTT/webhook devices — can feed sanitized coarse claims into the **same unchanged**
//! `Kernel::append_event_checked` choke point.
//!
//! # Trust boundary
//!
//! Adapters run **outside the kernel TCB**, exactly like `frigate_bridge` today, and are treated
//! as careless or malicious. They are *less* privileged than a module: they produce only
//! [`Claim`] values and cannot open the database, network, or filesystem unless they declare the
//! capability. The [`SensorAdapter`] trait is an **audit boundary** (cf.
//! `detect::backend::DetectorBackend`); the **security boundary** is the kernel's three gates
//! inside `append_event_checked` (event-type allowlist, contract enforcer, zone policy).
//!
//! See `spec/sensor_adapter_contract_v0.md` and `spec/witness_mesh_os_v0.md`.

pub mod contract;
pub mod host;
pub mod observability;
pub mod registry;

#[cfg(feature = "adapter-sandbox")]
pub mod sandbox;

#[cfg(feature = "adapter-ble-presence")]
pub mod ble_presence;
#[cfg(feature = "adapter-frigate")]
pub mod frigate;
#[cfg(feature = "adapter-mqtt-sensor")]
pub mod mqtt_sensor;
#[cfg(feature = "adapter-webhook")]
pub mod webhook;

pub use contract::{
    claim_kind_to_event_type, claim_to_candidate, AdapterDescriptor, Claim, ClaimKind,
};
pub use host::{AdapterHost, AdapterHostConfig};
pub use registry::AdapterRegistry;

use anyhow::Result;

/// A sensor adapter: an untrusted producer of vendor-neutral [`Claim`]s.
///
/// Implementations MUST be manually audited (this is an audit boundary) to ensure they:
/// - never retain or emit raw media / waveforms / images,
/// - never emit identity (plates, faces, embeddings, stable IDs) or precise time/location,
/// - never write to the sealed log directly (the host does, via the contract enforcer).
pub trait SensorAdapter: Send {
    /// Stable adapter name (used as the registry key).
    fn name(&self) -> &'static str;

    /// Static descriptor declaring the closed set of claim kinds / event types this adapter
    /// may emit and the capabilities it requests.
    fn descriptor(&self) -> &'static AdapterDescriptor;

    /// Pull or drain one batch of pre-sanitized claims.
    ///
    /// MUST NOT block indefinitely, MUST NOT touch disk/network/log, and MUST NOT emit raw
    /// media or identifiers. Returning an empty `Vec` is normal (no new signals this cycle).
    fn poll(&mut self) -> Result<Vec<Claim>>;

    /// Optional warm-up hook (e.g. open a connection). Default is a no-op.
    fn warm_up(&mut self) -> Result<()> {
        Ok(())
    }
}
