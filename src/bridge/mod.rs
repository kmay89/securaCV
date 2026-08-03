//! Egress bridges — projecting coarse witness state into home-automation
//! ecosystems.
//!
//! A *bridge* is deliberately not an [`adapter`](crate::adapter): an adapter
//! brings sensor claims **in**, a bridge sends coarse state **out**. That
//! direction is what makes bridges a metadata-minimization problem rather
//! than an ingest-integrity one, so every bridge in this module shares one
//! rule:
//!
//! > A bridge may publish only a closed vocabulary of present-tense
//! > booleans, on a **fixed cadence** that does not vary with event
//! > occurrence.
//!
//! The design of record is `docs/design/apple_home_integration.md`.

#[cfg(feature = "bridge-homekit")]
pub mod homekit;
