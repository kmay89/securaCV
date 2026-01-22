//! Break-glass core types and CLI.

mod core;

pub mod cli;

pub use cli::run;
pub use core::{
    Approval, BreakGlass, BreakGlassOutcome, BreakGlassReceipt, BreakGlassToken, QuorumPolicy,
    TrusteeEntry, TrusteeId, UnlockRequest,
};
