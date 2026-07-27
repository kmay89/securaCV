//! Perception eval harness.
//!
//! Measures detection *quality* (precision / recall / F1 / AP, a confidence-threshold sweep,
//! size-class confusion, and per-frame latency) for any [`crate::detect::DetectorBackend`]
//! against a labeled dataset. This is the missing counterpart to the kernel's integrity
//! guarantees: the crypto proves a sealed event was not *forged*; this proves the detector did
//! not silently *miss* it.
//!
//! - [`metrics`] — pure, always-compiled, unit-tested metric math (no model/image deps).
//! - [`dataset`] — `labels.json` manifest types and loader.
//! - `runner` — end-to-end run (feature `detect-eval`: needs `image` + `backend-tract`).

pub mod dataset;
pub mod metrics;

#[cfg(feature = "detect-eval")]
pub mod runner;

pub use metrics::{
    average_precision, build_report, confidence_sweep, parse_sweep_spec, ClassMetrics, Counts,
    EvalBox, EvalReport, FrameSample, GtObject, LatencyStats, OverallMetrics, Prediction,
    ReportMeta, SizeClassConfusion, SweepPoint,
};

#[cfg(feature = "detect-eval")]
pub use runner::{run_eval, EvalConfig};
