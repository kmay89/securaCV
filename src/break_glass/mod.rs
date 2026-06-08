//! Break-glass core types and CLI.

mod core;

pub mod cli;
pub mod http;
pub mod session;

pub use cli::run;
pub use core::{
    approvals_commitment, Approval, BreakGlass, BreakGlassOutcome, BreakGlassReceipt,
    BreakGlassToken, BreakGlassTokenFile, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
pub use http::{handle_break_glass, BreakGlassOps, HttpReply};
pub use session::{ApprovalRejection, BreakGlassSession, SessionStatus};
