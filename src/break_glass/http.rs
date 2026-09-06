//! HTTP request handling for the kernel-served break-glass web UI.
//!
//! This is the security-critical decision layer of the live request→approve→
//! unseal flow, kept transport-agnostic so it is unit-tested in isolation: it
//! takes a parsed method/path/body plus the in-memory [`BreakGlassSession`] and a
//! [`BreakGlassOps`] backend, and returns a secret-free JSON [`HttpReply`]. The
//! TCP layer (auth, loopback, TLS, opt-in) wraps this; the kernel/vault work is
//! behind the trait.
//!
//! Invariants enforced here (independent of the transport):
//! - The hash a trustee signs comes from the **server's** open session, never the
//!   client request — a caller cannot redirect approvals at a different hash.
//! - `unseal` proceeds only when the collected quorum is `ready`.
//! - The unsealed envelope is written to a **server-configured** directory; the
//!   client never supplies a path, so there is no path-traversal surface.
//! - No response body ever contains the token nonce or any cleartext — only the
//!   public outcome, the request hash to sign, and the written file path.

use std::path::PathBuf;

use anyhow::Result;
use serde::Deserialize;
use serde_json::json;

use super::core::{Approval, QuorumPolicy, TrusteeId, UnlockRequest};
use super::session::BreakGlassSession;
use crate::TimeBucket;

/// Upper bound on operator-supplied strings, to keep request bodies bounded.
const MAX_FIELD_LEN: usize = 512;

/// A ready-to-send JSON reply. `status` is the HTTP status code.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HttpReply {
    pub status: u16,
    pub body: String,
}

impl HttpReply {
    fn json(status: u16, value: serde_json::Value) -> Self {
        Self {
            status,
            body: value.to_string(),
        }
    }

    fn error(status: u16, code: &str) -> Self {
        Self::json(status, json!({ "error": code }))
    }
}

/// Kernel/vault operations the break-glass flow needs. Abstracted so the handler
/// is testable without a real database or vault; the production implementation
/// (in the server binary) wraps `Kernel` + `Vault`.
pub trait BreakGlassOps {
    /// The configured quorum policy, or `None` if break-glass is not provisioned.
    fn policy(&self) -> Result<Option<QuorumPolicy>>;

    /// The kernel's ruleset hash, bound into every [`UnlockRequest`].
    fn ruleset_hash(&self) -> [u8; 32];

    /// Authorize the collected approvals (logging a tamper-evident receipt either
    /// way), sign the resulting token, and unseal the envelope to `output_dir`.
    /// Returns the written file path on a granted unseal, or an error describing
    /// the denial. Must not return cleartext.
    fn authorize_unseal(
        &mut self,
        request: &UnlockRequest,
        approvals: &[Approval],
        now_bucket: TimeBucket,
        output_dir: &str,
    ) -> Result<PathBuf>;
}

#[derive(Deserialize)]
struct OpenReq {
    envelope: String,
    purpose: String,
    /// §3.6 operator context — optional at this wire so pre-§3.6 clients
    /// (the HA add-on wizard among them) keep working; when absent the
    /// receipt records the disclosure without a named requester.
    #[serde(default)]
    requested_by: Option<String>,
    #[serde(default)]
    reason: Option<String>,
    #[serde(default)]
    case_ref: Option<String>,
}

#[derive(Deserialize)]
struct ApproveReq {
    trustee: String,
    /// hex-encoded Ed25519 signature over the session's request hash.
    signature: String,
}

/// Route and handle one break-glass request. `path` is the full request path
/// (e.g. `/breakglass/request`); `output_dir` is the server-configured directory
/// unsealed envelopes are written to.
pub fn handle_break_glass<O: BreakGlassOps>(
    ops: &mut O,
    session: &mut BreakGlassSession,
    output_dir: &str,
    now_bucket: TimeBucket,
    method: &str,
    path: &str,
    body: &[u8],
) -> HttpReply {
    let policy = match ops.policy() {
        Ok(Some(p)) => p,
        Ok(None) => return HttpReply::error(409, "policy_not_configured"),
        Err(_) => return HttpReply::error(500, "policy_load_failed"),
    };

    match (method, path) {
        ("GET", "/breakglass/policy") => policy_reply(&policy),
        ("POST", "/breakglass/request") => open_request(ops, session, &policy, now_bucket, body),
        ("GET", "/breakglass/status") => status_reply(session, &policy),
        ("POST", "/breakglass/approve") => submit_approval(session, &policy, body),
        ("POST", "/breakglass/unseal") => unseal(ops, session, &policy, output_dir, now_bucket),
        ("POST", "/breakglass/close") => {
            session.close();
            HttpReply::json(200, json!({ "closed": true }))
        }
        // Known resource, wrong verb vs. unknown path.
        (_, p) if p.starts_with("/breakglass/") => HttpReply::error(405, "method_not_allowed"),
        _ => HttpReply::error(404, "not_found"),
    }
}

fn policy_reply(policy: &QuorumPolicy) -> HttpReply {
    let trustees: Vec<_> = policy
        .trustees
        .iter()
        .map(|t| json!({ "id": t.id.0, "public_key": hex::encode(t.public_key) }))
        .collect();
    HttpReply::json(
        200,
        json!({
            "n": policy.n,
            "m": policy.m,
            "trustees": trustees,
            // The closed §3.6 reason vocabulary, served so the console's
            // dropdown has ONE source of truth (core::REASON_CODES) and the
            // page never hardcodes a copy.
            "reason_codes": crate::break_glass::REASON_CODES,
        }),
    )
}

fn open_request<O: BreakGlassOps>(
    ops: &O,
    session: &mut BreakGlassSession,
    policy: &QuorumPolicy,
    now_bucket: TimeBucket,
    body: &[u8],
) -> HttpReply {
    let req: OpenReq = match serde_json::from_slice(body) {
        Ok(r) => r,
        Err(_) => return HttpReply::error(400, "invalid_json"),
    };
    if req.envelope.len() > MAX_FIELD_LEN || req.purpose.len() > MAX_FIELD_LEN {
        return HttpReply::error(400, "field_too_long");
    }
    if [&req.requested_by, &req.reason, &req.case_ref]
        .iter()
        .any(|f| f.as_deref().is_some_and(|v| v.len() > MAX_FIELD_LEN))
    {
        return HttpReply::error(400, "field_too_long");
    }
    let request =
        match UnlockRequest::new(&req.envelope, ops.ruleset_hash(), &req.purpose, now_bucket) {
            Ok(r) => r,
            Err(_) => return HttpReply::error(400, "invalid_request"),
        };
    // §3.6 operator context: requested_by and reason travel together (both
    // bind into the hash trustees sign); a partial pair is a caller bug, not
    // something to guess about.
    let request = match (&req.requested_by, &req.reason) {
        (Some(requested_by), Some(reason)) => {
            let context = match crate::break_glass::OperatorContext::new(
                requested_by,
                reason,
                req.case_ref.as_deref(),
                None,
            ) {
                Ok(c) => c,
                Err(_) => return HttpReply::error(400, "invalid_operator_context"),
            };
            match request.with_context(context) {
                Ok(r) => r,
                Err(_) => return HttpReply::error(400, "invalid_operator_context"),
            }
        }
        (None, None) => request,
        _ => return HttpReply::error(400, "partial_operator_context"),
    };
    let request_hash = session.open(request);
    HttpReply::json(
        200,
        json!({
            "request_hash": hex::encode(request_hash),
            "time_bucket": { "start_epoch_s": now_bucket.start_epoch_s, "size_s": now_bucket.size_s },
            "needed": policy.n,
        }),
    )
}

fn status_reply(session: &BreakGlassSession, policy: &QuorumPolicy) -> HttpReply {
    match session.status(policy) {
        None => HttpReply::error(404, "no_open_request"),
        Some(s) => HttpReply::json(
            200,
            json!({
                "envelope": s.envelope,
                "purpose": s.purpose,
                "request_hash": s.request_hash_hex,
                "needed": s.needed,
                "collected": s.collected,
                "ready": s.ready,
            }),
        ),
    }
}

fn submit_approval(
    session: &mut BreakGlassSession,
    policy: &QuorumPolicy,
    body: &[u8],
) -> HttpReply {
    let req: ApproveReq = match serde_json::from_slice(body) {
        Ok(r) => r,
        Err(_) => return HttpReply::error(400, "invalid_json"),
    };
    if req.trustee.len() > MAX_FIELD_LEN {
        return HttpReply::error(400, "field_too_long");
    }
    let signature = match hex::decode(req.signature.trim()) {
        Ok(s) => s,
        Err(_) => return HttpReply::error(400, "invalid_signature_hex"),
    };
    // The hash the trustee must sign is taken from the open session, not the
    // client — so a caller cannot point an approval at a different request.
    let request_hash = match session.request() {
        Some(r) => r.request_hash(),
        None => return HttpReply::error(409, "no_open_request"),
    };
    let approval = Approval::new(TrusteeId::new(&req.trustee), request_hash, signature);
    match session.submit(policy, approval) {
        Ok(replaced_existing) => {
            let status = session.status(policy);
            HttpReply::json(
                200,
                json!({
                    "accepted": true,
                    "replaced": !replaced_existing,
                    "collected": status.as_ref().map(|s| s.collected.clone()).unwrap_or_default(),
                    "needed": policy.n,
                    "ready": status.map(|s| s.ready).unwrap_or(false),
                }),
            )
        }
        Err(rejection) => HttpReply::json(
            422,
            json!({ "accepted": false, "reason": rejection.as_str() }),
        ),
    }
}

fn unseal<O: BreakGlassOps>(
    ops: &mut O,
    session: &mut BreakGlassSession,
    policy: &QuorumPolicy,
    output_dir: &str,
    now_bucket: TimeBucket,
) -> HttpReply {
    let status = match session.status(policy) {
        Some(s) => s,
        None => return HttpReply::error(409, "no_open_request"),
    };
    if !status.ready {
        return HttpReply::json(
            409,
            json!({
                "error": "quorum_not_met",
                "collected": status.collected,
                "needed": status.needed,
            }),
        );
    }
    // Clone the request before borrowing approvals immutably for the ops call.
    let request = match session.request() {
        Some(r) => r.clone(),
        None => return HttpReply::error(409, "no_open_request"),
    };
    match ops.authorize_unseal(&request, session.approvals(), now_bucket, output_dir) {
        Ok(path) => {
            // Single-use: the request is consumed once unsealed.
            session.close();
            HttpReply::json(
                200,
                json!({
                    "outcome": "granted",
                    "envelope": request.vault_envelope_id,
                    "output_path": path.to_string_lossy(),
                }),
            )
        }
        Err(e) => HttpReply::json(403, json!({ "outcome": "denied", "reason": e.to_string() })),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{TrusteeEntry, TrusteeId};
    use ed25519_dalek::SigningKey;

    const RULESET: [u8; 32] = [7u8; 32];

    fn bucket() -> TimeBucket {
        TimeBucket {
            start_epoch_s: 1000,
            size_s: 600,
        }
    }

    struct MockOps {
        policy: Option<QuorumPolicy>,
        unseal_calls: usize,
        last_approval_count: usize,
    }

    impl MockOps {
        fn new(policy: QuorumPolicy) -> Self {
            Self {
                policy: Some(policy),
                unseal_calls: 0,
                last_approval_count: 0,
            }
        }
    }

    impl BreakGlassOps for MockOps {
        fn policy(&self) -> Result<Option<QuorumPolicy>> {
            Ok(self.policy.clone())
        }
        fn ruleset_hash(&self) -> [u8; 32] {
            RULESET
        }
        fn authorize_unseal(
            &mut self,
            request: &UnlockRequest,
            approvals: &[Approval],
            _now_bucket: TimeBucket,
            output_dir: &str,
        ) -> Result<PathBuf> {
            self.unseal_calls += 1;
            self.last_approval_count = approvals.len();
            Ok(PathBuf::from(output_dir).join(format!("{}.raw", request.vault_envelope_id)))
        }
    }

    fn two_of_two() -> (SigningKey, SigningKey, QuorumPolicy) {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let policy = QuorumPolicy::new(
            2,
            vec![
                TrusteeEntry {
                    id: TrusteeId::new("alice"),
                    public_key: alice.verifying_key().to_bytes(),
                },
                TrusteeEntry {
                    id: TrusteeId::new("bob"),
                    public_key: bob.verifying_key().to_bytes(),
                },
            ],
        )
        .unwrap();
        (alice, bob, policy)
    }

    fn approve_body(key: &SigningKey, trustee: &str, request_hash_hex: &str) -> Vec<u8> {
        let rh: [u8; 32] = hex::decode(request_hash_hex).unwrap().try_into().unwrap();
        let sig = crate::break_glass::sign_approval(key, &rh);
        json!({ "trustee": trustee, "signature": hex::encode(sig) })
            .to_string()
            .into_bytes()
    }

    fn open(ops: &mut MockOps, session: &mut BreakGlassSession) -> String {
        let body = json!({ "envelope": "vault:1", "purpose": "incident" })
            .to_string()
            .into_bytes();
        let reply = handle_break_glass(
            ops,
            session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/request",
            &body,
        );
        assert_eq!(reply.status, 200);
        let v: serde_json::Value = serde_json::from_str(&reply.body).unwrap();
        assert_eq!(v["needed"], 2);
        v["request_hash"].as_str().unwrap().to_string()
    }

    /// §3.6 over the wire: context fields ride into the session's request
    /// (so the eventual receipt records them), the policy reply serves the
    /// closed reason vocabulary, and a partial pair is refused.
    #[test]
    fn open_request_carries_operator_context() {
        let (_alice, _bob, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();

        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "GET",
            "/breakglass/policy",
            b"",
        );
        assert_eq!(reply.status, 200);
        let v: serde_json::Value = serde_json::from_str(&reply.body).unwrap();
        assert_eq!(
            v["reason_codes"]
                .as_array()
                .expect("policy reply serves the reason vocabulary")
                .len(),
            crate::break_glass::REASON_CODES.len()
        );

        let body = json!({
            "envelope": "vault:1",
            "purpose": "incident",
            "requested_by": "Alice Operator",
            "reason": "incident-review",
            "case_ref": "case-9"
        })
        .to_string()
        .into_bytes();
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/request",
            &body,
        );
        assert_eq!(reply.status, 200, "{}", reply.body);
        let stored = session.request().expect("session holds the request");
        let context = stored.context.as_ref().expect("context recorded");
        assert_eq!(context.requester_name, "Alice Operator");
        assert_eq!(context.reason_code, "incident-review");
        assert_eq!(context.case_ref.as_deref(), Some("case-9"));

        // requested_by without reason: refused, nothing opened over it.
        let mut session2 = BreakGlassSession::new();
        let partial = json!({
            "envelope": "vault:1",
            "purpose": "incident",
            "requested_by": "Alice Operator"
        })
        .to_string()
        .into_bytes();
        let reply = handle_break_glass(
            &mut ops,
            &mut session2,
            "/out",
            bucket(),
            "POST",
            "/breakglass/request",
            &partial,
        );
        assert_eq!(reply.status, 400);
        assert!(reply.body.contains("partial_operator_context"));

        // An unknown reason code is refused too — the vocabulary is closed.
        let bogus = json!({
            "envelope": "vault:1",
            "purpose": "incident",
            "requested_by": "Alice Operator",
            "reason": "definitely-not-a-code"
        })
        .to_string()
        .into_bytes();
        let reply = handle_break_glass(
            &mut ops,
            &mut session2,
            "/out",
            bucket(),
            "POST",
            "/breakglass/request",
            &bogus,
        );
        assert_eq!(reply.status, 400);
        assert!(reply.body.contains("invalid_operator_context"));
    }

    #[test]
    fn policy_not_configured_blocks_everything() {
        let mut ops = MockOps {
            policy: None,
            unseal_calls: 0,
            last_approval_count: 0,
        };
        let mut session = BreakGlassSession::new();
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "GET",
            "/breakglass/policy",
            b"",
        );
        assert_eq!(reply.status, 409);
        assert!(reply.body.contains("policy_not_configured"));
    }

    #[test]
    fn policy_listing_exposes_pubkeys_not_secrets() {
        let (_, _, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "GET",
            "/breakglass/policy",
            b"",
        );
        assert_eq!(reply.status, 200);
        let v: serde_json::Value = serde_json::from_str(&reply.body).unwrap();
        assert_eq!(v["n"], 2);
        assert_eq!(v["m"], 2);
        assert_eq!(v["trustees"].as_array().unwrap().len(), 2);
        assert_eq!(v["trustees"][0]["id"], "alice");
    }

    #[test]
    fn full_flow_request_approve_unseal() {
        let (alice, bob, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();
        let rh = open(&mut ops, &mut session);

        // First approval — accepted, not yet ready.
        let r1 = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/approve",
            &approve_body(&alice, "alice", &rh),
        );
        assert_eq!(r1.status, 200);
        let v1: serde_json::Value = serde_json::from_str(&r1.body).unwrap();
        assert_eq!(v1["accepted"], true);
        assert_eq!(v1["ready"], false);

        // Unseal before quorum is refused, and must not call the backend.
        let early = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/unseal",
            b"",
        );
        assert_eq!(early.status, 409);
        assert!(early.body.contains("quorum_not_met"));
        assert_eq!(ops.unseal_calls, 0);

        // Second approval reaches quorum.
        let r2 = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/approve",
            &approve_body(&bob, "bob", &rh),
        );
        let v2: serde_json::Value = serde_json::from_str(&r2.body).unwrap();
        assert_eq!(v2["ready"], true);

        // Unseal now succeeds, passes both approvals, writes under the server dir.
        let done = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/unseal",
            b"",
        );
        assert_eq!(done.status, 200);
        let dv: serde_json::Value = serde_json::from_str(&done.body).unwrap();
        assert_eq!(dv["outcome"], "granted");
        assert_eq!(dv["output_path"], "/out/vault:1.raw");
        assert_eq!(ops.unseal_calls, 1);
        assert_eq!(ops.last_approval_count, 2);
        // Single-use: the session is closed after a granted unseal.
        assert!(!session.is_open());
    }

    #[test]
    fn approval_with_wrong_key_is_rejected_unprocessable() {
        let (_alice, _bob, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();
        let rh = open(&mut ops, &mut session);
        // Mallory signs but claims to be alice.
        let mallory = SigningKey::from_bytes(&[9u8; 32]);
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/approve",
            &approve_body(&mallory, "alice", &rh),
        );
        assert_eq!(reply.status, 422);
        assert!(reply.body.contains("bad_signature"));
    }

    #[test]
    fn approve_without_open_request_is_conflict() {
        let (alice, _bob, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/approve",
            &approve_body(&alice, "alice", &hex::encode([0u8; 32])),
        );
        assert_eq!(reply.status, 409);
        assert!(reply.body.contains("no_open_request"));
    }

    #[test]
    fn unknown_path_and_wrong_method() {
        let (_, _, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();
        let nf = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "GET",
            "/nope",
            b"",
        );
        assert_eq!(nf.status, 404);
        let wm = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "DELETE",
            "/breakglass/request",
            b"",
        );
        assert_eq!(wm.status, 405);
    }

    #[test]
    fn malformed_json_is_bad_request() {
        let (_, _, policy) = two_of_two();
        let mut ops = MockOps::new(policy);
        let mut session = BreakGlassSession::new();
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/request",
            b"{not json",
        );
        assert_eq!(reply.status, 400);
        assert!(reply.body.contains("invalid_json"));
    }
}
