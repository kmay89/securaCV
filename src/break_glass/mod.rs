//! Break-glass core types and CLI.

mod core;

pub mod cli;

pub use cli::run;
pub use core::{
    approvals_commitment, Approval, BreakGlass, BreakGlassOutcome, BreakGlassReceipt,
    BreakGlassToken, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
