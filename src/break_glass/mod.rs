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
    approvals_commitment, count_valid_distinct_approvals,
    count_valid_distinct_policy_change_approvals, sign_approval, sign_policy_change_approval,
    verify_approval, verify_policy_change_approval, verify_trustee_attribution, Approval,
    BreakGlass, BreakGlassOutcome, BreakGlassReceipt, BreakGlassToken, BreakGlassTokenFile,
    OperatorContext, PolicyChangeProposal, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
    REASON_CODES,
};
pub use http::{handle_break_glass, BreakGlassOps, HttpReply};
pub use server::{BreakGlassServer, BreakGlassServerConfig};
pub use session::{ApprovalRejection, BreakGlassSession, SessionStatus};
