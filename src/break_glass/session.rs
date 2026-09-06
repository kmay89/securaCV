//! In-memory break-glass session: accumulates trustee approvals for a single
//! pending [`UnlockRequest`] until the quorum is met.
//!
//! The CLI flow is stateless (each approval is a file on disk). A *served* UI
//! needs somewhere to collect approvals as trustees submit them over time, then
//! hand the assembled set to `BreakGlass::authorize`. This module is exactly
//! that — and nothing more. It holds **no secrets**: approvals are
//! self-authenticating Ed25519 signatures over the public request hash, so the
//! session is safe to keep in memory and is wiped when the request is closed.
//!
//! It is deliberately pure logic (no I/O, no network) so the authorization rules
//! are unit-tested in isolation; the HTTP layer is a thin wrapper over this.

use crate::TimeBucket;
use ed25519_dalek::VerifyingKey;

use super::core::{verify_approval, Approval, QuorumPolicy, UnlockRequest};

/// Why a submitted approval was not added to the session.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ApprovalRejection {
    /// No request is currently open.
    NoOpenRequest,
    /// The approval's `request_hash` does not match the open request.
    WrongRequest,
    /// The trustee id is not in the active quorum policy.
    UnknownTrustee,
    /// The signature did not verify against the trustee's public key.
    BadSignature,
    /// The approval's stored public key bytes are not a valid Ed25519 key.
    InvalidTrusteeKey,
}

impl ApprovalRejection {
    pub fn as_str(&self) -> &'static str {
        match self {
            ApprovalRejection::NoOpenRequest => "no_open_request",
            ApprovalRejection::WrongRequest => "wrong_request",
            ApprovalRejection::UnknownTrustee => "unknown_trustee",
            ApprovalRejection::BadSignature => "bad_signature",
            ApprovalRejection::InvalidTrusteeKey => "invalid_trustee_key",
        }
    }
}

/// A snapshot of where a session stands, for status display. Contains no secrets.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionStatus {
    pub envelope: String,
    pub purpose: String,
    /// §3.6 operator context bound into the request hash, when the request
    /// carried it — surfaced so the served wire can SHOW trustees what the
    /// hash they sign covers (WYSIWYS), not just the hash.
    pub requested_by: Option<String>,
    pub reason: Option<String>,
    pub case_ref: Option<String>,
    /// hex-encoded ruleset hash the request binds (a trustee recomputing the
    /// request hash needs it).
    pub ruleset_hash_hex: String,
    pub time_bucket: TimeBucket,
    /// hex-encoded request hash trustees must sign.
    pub request_hash_hex: String,
    /// Quorum threshold (n).
    pub needed: u8,
    /// Trustee ids whose valid approvals have been collected (sorted, unique).
    pub collected: Vec<String>,
    /// True once `collected.len() >= needed` — authorization will succeed.
    pub ready: bool,
}

struct Pending {
    request: UnlockRequest,
    request_hash: [u8; 32],
    approvals: Vec<Approval>,
}

/// Holds at most one open request and its collected approvals.
#[derive(Default)]
pub struct BreakGlassSession {
    pending: Option<Pending>,
}

impl BreakGlassSession {
    pub fn new() -> Self {
        Self::default()
    }

    /// True if a request is currently open.
    pub fn is_open(&self) -> bool {
        self.pending.is_some()
    }

    /// Open a new request, discarding any previous pending request and its
    /// collected approvals. Returns the request hash trustees must sign.
    pub fn open(&mut self, request: UnlockRequest) -> [u8; 32] {
        let request_hash = request.request_hash();
        self.pending = Some(Pending {
            request,
            request_hash,
            approvals: Vec::new(),
        });
        request_hash
    }

    /// Discard the open request and wipe collected approvals.
    pub fn close(&mut self) {
        self.pending = None;
    }

    /// Validate a submitted approval against the active policy and, if valid,
    /// add it (deduplicated by trustee — a trustee's latest valid approval wins).
    /// Returns `Ok(true)` if newly added, `Ok(false)` if it replaced an existing
    /// approval from the same trustee, or `Err` with the reason it was rejected.
    pub fn submit(
        &mut self,
        policy: &QuorumPolicy,
        approval: Approval,
    ) -> Result<bool, ApprovalRejection> {
        let pending = self
            .pending
            .as_mut()
            .ok_or(ApprovalRejection::NoOpenRequest)?;

        if approval.request_hash != pending.request_hash {
            return Err(ApprovalRejection::WrongRequest);
        }
        let trustee = policy
            .trustees
            .iter()
            .find(|t| t.id.0 == approval.trustee.0)
            .ok_or(ApprovalRejection::UnknownTrustee)?;
        // Surface an unusable trustee key distinctly from a bad signature.
        VerifyingKey::from_bytes(&trustee.public_key)
            .map_err(|_| ApprovalRejection::InvalidTrusteeKey)?;
        // Domain-separated: a bare-hash or cross-context signature is rejected.
        if !verify_approval(
            &trustee.public_key,
            &pending.request_hash,
            &approval.signature,
        ) {
            return Err(ApprovalRejection::BadSignature);
        }

        // Dedupe by trustee id: replace any prior approval from this trustee.
        if let Some(slot) = pending
            .approvals
            .iter_mut()
            .find(|a| a.trustee.0 == approval.trustee.0)
        {
            *slot = approval;
            Ok(false)
        } else {
            pending.approvals.push(approval);
            Ok(true)
        }
    }

    /// The approvals collected so far (already validated on submit).
    pub fn approvals(&self) -> &[Approval] {
        self.pending
            .as_ref()
            .map(|p| p.approvals.as_slice())
            .unwrap_or(&[])
    }

    /// The open request, if any.
    pub fn request(&self) -> Option<&UnlockRequest> {
        self.pending.as_ref().map(|p| &p.request)
    }

    /// A secret-free status snapshot for display, or `None` if no request is open.
    pub fn status(&self, policy: &QuorumPolicy) -> Option<SessionStatus> {
        let pending = self.pending.as_ref()?;
        let mut collected: Vec<String> = pending
            .approvals
            .iter()
            .map(|a| a.trustee.0.clone())
            .collect();
        collected.sort();
        collected.dedup();
        let context = pending.request.context.as_ref();
        Some(SessionStatus {
            envelope: pending.request.vault_envelope_id.clone(),
            purpose: pending.request.purpose.clone(),
            requested_by: context.map(|c| c.requester_name.clone()),
            reason: context.map(|c| c.reason_code.clone()),
            case_ref: context.and_then(|c| c.case_ref.clone()),
            ruleset_hash_hex: hex::encode(pending.request.ruleset_hash),
            time_bucket: pending.request.time_bucket,
            request_hash_hex: hex::encode(pending.request_hash),
            needed: policy.n,
            ready: collected.len() >= policy.n as usize,
            collected,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{TrusteeEntry, TrusteeId};
    use crate::TimeBucket;
    use ed25519_dalek::SigningKey;

    fn bucket() -> TimeBucket {
        TimeBucket {
            start_epoch_s: 1000,
            size_s: 600,
        }
    }

    fn policy_with(keys: &[(&str, &SigningKey)], threshold: u8) -> QuorumPolicy {
        let trustees = keys
            .iter()
            .map(|(id, k)| TrusteeEntry {
                id: TrusteeId::new(id),
                public_key: k.verifying_key().to_bytes(),
            })
            .collect();
        QuorumPolicy::new(threshold, trustees).unwrap()
    }

    fn sign(key: &SigningKey, id: &str, request_hash: [u8; 32]) -> Approval {
        Approval::signed(TrusteeId::new(id), request_hash, key)
    }

    #[test]
    fn submit_before_open_is_rejected() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let policy = policy_with(&[("alice", &alice)], 1);
        let mut session = BreakGlassSession::new();
        let approval = sign(&alice, "alice", [9u8; 32]);
        assert_eq!(
            session.submit(&policy, approval),
            Err(ApprovalRejection::NoOpenRequest)
        );
    }

    #[test]
    fn valid_approval_reaches_quorum() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let policy = policy_with(&[("alice", &alice), ("bob", &bob)], 2);
        let mut session = BreakGlassSession::new();
        let req = UnlockRequest::new("vault:1", [3u8; 32], "incident", bucket()).unwrap();
        let rh = session.open(req);

        assert!(!session.status(&policy).unwrap().ready);
        assert_eq!(session.submit(&policy, sign(&alice, "alice", rh)), Ok(true));
        assert!(!session.status(&policy).unwrap().ready); // 1 of 2
        assert_eq!(session.submit(&policy, sign(&bob, "bob", rh)), Ok(true));

        let status = session.status(&policy).unwrap();
        assert!(status.ready); // 2 of 2
        assert_eq!(status.collected, vec!["alice", "bob"]);
        assert_eq!(status.needed, 2);
        assert_eq!(session.approvals().len(), 2);
    }

    #[test]
    fn unknown_trustee_and_bad_signature_rejected() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let mallory = SigningKey::from_bytes(&[7u8; 32]);
        let policy = policy_with(&[("alice", &alice)], 1);
        let mut session = BreakGlassSession::new();
        let rh = session.open(UnlockRequest::new("vault:1", [3u8; 32], "x", bucket()).unwrap());

        // Unknown trustee id.
        assert_eq!(
            session.submit(&policy, sign(&mallory, "mallory", rh)),
            Err(ApprovalRejection::UnknownTrustee)
        );
        // Known id but signature from the wrong key.
        // Known id, but the signature is from the wrong key (correct domain).
        let forged = Approval::signed(TrusteeId::new("alice"), rh, &mallory);
        assert_eq!(
            session.submit(&policy, forged),
            Err(ApprovalRejection::BadSignature)
        );
        assert!(session.approvals().is_empty());
    }

    #[test]
    fn wrong_request_hash_rejected() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let policy = policy_with(&[("alice", &alice)], 1);
        let mut session = BreakGlassSession::new();
        session.open(UnlockRequest::new("vault:1", [3u8; 32], "x", bucket()).unwrap());
        // Sign some other hash entirely.
        assert_eq!(
            session.submit(&policy, sign(&alice, "alice", [42u8; 32])),
            Err(ApprovalRejection::WrongRequest)
        );
    }

    #[test]
    fn duplicate_trustee_counts_once_and_replaces() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let policy = policy_with(&[("alice", &alice)], 1);
        let mut session = BreakGlassSession::new();
        let rh = session.open(UnlockRequest::new("vault:1", [3u8; 32], "x", bucket()).unwrap());
        assert_eq!(session.submit(&policy, sign(&alice, "alice", rh)), Ok(true));
        // Same trustee again → replaces, not a second count.
        assert_eq!(
            session.submit(&policy, sign(&alice, "alice", rh)),
            Ok(false)
        );
        assert_eq!(session.approvals().len(), 1);
        assert_eq!(session.status(&policy).unwrap().collected, vec!["alice"]);
    }

    #[test]
    fn close_wipes_state() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let policy = policy_with(&[("alice", &alice)], 1);
        let mut session = BreakGlassSession::new();
        let rh = session.open(UnlockRequest::new("vault:1", [3u8; 32], "x", bucket()).unwrap());
        session.submit(&policy, sign(&alice, "alice", rh)).unwrap();
        assert!(session.is_open());
        session.close();
        assert!(!session.is_open());
        assert!(session.status(&policy).is_none());
        assert!(session.approvals().is_empty());
    }
}
