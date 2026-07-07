//! Break-glass core types and CLI.

mod core;

pub mod backend;
pub mod cli;
pub mod http;
pub mod server;
pub mod session;

pub use backend::KernelVaultOps;
pub use cli::run;
pub use core::{
    approvals_commitment, sign_approval, verify_approval, Approval, BreakGlass, BreakGlassOutcome,
    BreakGlassReceipt, BreakGlassToken, BreakGlassTokenFile, QuorumPolicy, TrusteeEntry, TrusteeId,
    UnlockRequest,
};
pub use http::{handle_break_glass, BreakGlassOps, HttpReply};
pub use server::{BreakGlassServer, BreakGlassServerConfig};
pub use session::{ApprovalRejection, BreakGlassSession, SessionStatus};
