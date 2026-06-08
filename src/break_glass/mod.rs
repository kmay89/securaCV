//! Break-glass core types and CLI.

mod core;

pub mod cli;
pub mod session;

pub use cli::run;
pub use session::{ApprovalRejection, BreakGlassSession, SessionStatus};
pub use core::{
    approvals_commitment, Approval, BreakGlass, BreakGlassOutcome, BreakGlassReceipt,
    BreakGlassToken, BreakGlassTokenFile, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
