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
//! - The quorum policy can be **bootstrapped** here (`POST /breakglass/policy`)
//!   only while none exists; once one does, the route answers 409 and every
//!   change goes through the quorum-consented CLI flow (`break_glass policy
//!   propose` / `approve` / `set --approvals`) — Invariant V.
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

use super::core::{Approval, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest};
use super::session::BreakGlassSession;
use crate::TimeBucket;

// One bound for every consent-bound text field, owned by core so the CLI,
// the request file, and this wire agree.
use crate::break_glass::MAX_FIELD_LEN;

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

/// What a policy bootstrap did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PolicyBootstrap {
    /// No policy existed; this one is now stored (with its bootstrap history row).
    Stored,
    /// A policy already exists. Nothing was written: changes need the current
    /// quorum's consent, which this route does not carry.
    AlreadyConfigured,
}

/// Kernel/vault operations the break-glass flow needs. Abstracted so the handler
/// is testable without a real database or vault; the production implementation
/// (in the server binary) wraps `Kernel` + `Vault`.
pub trait BreakGlassOps {
    /// The configured quorum policy, or `None` if break-glass is not provisioned.
    fn policy(&self) -> Result<Option<QuorumPolicy>>;

    /// Store the FIRST quorum policy — the bootstrap, which needs no approvals
    /// because there is no quorum yet to consent. Must never replace an
    /// existing policy: report [`PolicyBootstrap::AlreadyConfigured`] instead.
    /// The default refuses, for a backend that cannot store one.
    fn bootstrap_policy(&mut self, _policy: &QuorumPolicy) -> Result<PolicyBootstrap> {
        Err(anyhow::anyhow!(
            "this backend cannot store a quorum policy; use `break_glass policy set`"
        ))
    }

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

/// The bootstrap body: the shape `GET /breakglass/policy` serves, minus the
/// server-owned fields. `m` is optional and, when present, must equal the
/// trustee count; `crypto_mode` defaults to classical, as `policy set` does on
/// an empty database.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct BootstrapReq {
    n: u8,
    #[serde(default)]
    m: Option<u8>,
    trustees: Vec<BootstrapTrustee>,
    #[serde(default)]
    crypto_mode: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct BootstrapTrustee {
    id: String,
    /// hex-encoded 32-byte Ed25519 public key.
    public_key: String,
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
        Ok(p) => p,
        Err(_) => return HttpReply::error(500, "policy_load_failed"),
    };
    // The one route that runs before a policy exists — and only then.
    if (method, path) == ("POST", "/breakglass/policy") {
        return bootstrap_policy(ops, policy.as_ref(), body);
    }
    let policy = match policy {
        Some(p) => p,
        None => return HttpReply::error(409, "policy_not_configured"),
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

/// The 409 every bootstrap attempt gets once a policy exists: changes need the
/// current quorum's consent, which travels through the CLI, not this route.
fn already_configured() -> HttpReply {
    HttpReply::json(
        409,
        json!({
            "error": "policy_already_configured",
            "hint": "changes need the current quorum's consent: break_glass policy propose, \
                     policy approve, then policy set --approvals",
        }),
    )
}

/// `POST /breakglass/policy`: store the first quorum policy. Validated exactly
/// as `break_glass policy set` validates it (`QuorumPolicy::new`), and written
/// through the same quorum-gated kernel path, so the policy-history row is the
/// CLI's bootstrap row.
fn bootstrap_policy<O: BreakGlassOps>(
    ops: &mut O,
    current: Option<&QuorumPolicy>,
    body: &[u8],
) -> HttpReply {
    if current.is_some() {
        return already_configured();
    }
    let req: BootstrapReq = match serde_json::from_slice(body) {
        Ok(r) => r,
        Err(_) => return HttpReply::error(400, "invalid_json"),
    };
    if req.trustees.iter().any(|t| t.id.len() > MAX_FIELD_LEN) {
        return HttpReply::error(400, "field_too_long");
    }
    let invalid = |reason: String| {
        HttpReply::json(422, json!({ "error": "invalid_policy", "reason": reason }))
    };
    let mut entries = Vec::with_capacity(req.trustees.len());
    for trustee in &req.trustees {
        let key = match hex::decode(trustee.public_key.trim()) {
            Ok(bytes) => bytes,
            Err(_) => return invalid(format!("trustee {}: public key is not hex", trustee.id)),
        };
        let public_key: [u8; 32] = match key.try_into() {
            Ok(k) => k,
            Err(_) => {
                return invalid(format!(
                    "trustee {}: public key must be 32 bytes (64 hex characters)",
                    trustee.id
                ))
            }
        };
        entries.push(TrusteeEntry {
            id: TrusteeId::new(&trustee.id),
            public_key,
        });
    }
    if req.m.is_some_and(|m| m as usize != entries.len()) {
        return invalid("m does not match the number of trustees".to_string());
    }
    let mut policy = match QuorumPolicy::new(req.n, entries) {
        Ok(p) => p,
        Err(e) => return invalid(e.to_string()),
    };
    if let Some(mode) = req.crypto_mode.as_deref() {
        policy.vault.crypto_mode = match mode.parse() {
            Ok(m) => m,
            Err(e) => return invalid(format!("{e}")),
        };
    }
    match ops.bootstrap_policy(&policy) {
        Ok(PolicyBootstrap::Stored) => {
            let mut reply = policy_reply(&policy);
            reply.status = 201;
            reply
        }
        Ok(PolicyBootstrap::AlreadyConfigured) => already_configured(),
        Err(e) => HttpReply::json(
            500,
            json!({ "error": "policy_store_failed", "reason": e.to_string() }),
        ),
    }
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
        // A case reference without the requester/reason pair would be silently
        // dropped from the permanent record — the same caller bug as a
        // partial pair, refused the same way.
        (None, None) if req.case_ref.is_none() => request,
        _ => return HttpReply::error(400, "partial_operator_context"),
    };
    // Echo every field the hash binds — as NORMALIZED (trimmed) by
    // construction, since those are the bytes the hash covers — plus the
    // ruleset hash, so the console can hand trustees the full preimage of
    // the hash they sign, not a bare hash.
    let echo = json!({
        "envelope": request.vault_envelope_id,
        "purpose": request.purpose,
        "ruleset_hash": hex::encode(request.ruleset_hash),
        "time_bucket": { "start_epoch_s": now_bucket.start_epoch_s, "size_s": now_bucket.size_s },
        "requested_by": request.context.as_ref().map(|c| c.requester_name.clone()),
        "reason": request.context.as_ref().map(|c| c.reason_code.clone()),
        "case_ref": request.context.as_ref().and_then(|c| c.case_ref.clone()),
    });
    let request_hash = session.open(request);
    let mut reply = echo;
    reply["request_hash"] = json!(hex::encode(request_hash));
    reply["needed"] = json!(policy.n);
    HttpReply::json(200, reply)
}

fn status_reply(session: &BreakGlassSession, policy: &QuorumPolicy) -> HttpReply {
    match session.status(policy) {
        None => HttpReply::error(404, "no_open_request"),
        Some(s) => HttpReply::json(
            200,
            json!({
                "envelope": s.envelope,
                "purpose": s.purpose,
                "requested_by": s.requested_by,
                "reason": s.reason,
                "case_ref": s.case_ref,
                "ruleset_hash": s.ruleset_hash_hex,
                "time_bucket": { "start_epoch_s": s.time_bucket.start_epoch_s, "size_s": s.time_bucket.size_s },
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
        fn bootstrap_policy(&mut self, policy: &QuorumPolicy) -> Result<PolicyBootstrap> {
            if self.policy.is_some() {
                return Ok(PolicyBootstrap::AlreadyConfigured);
            }
            self.policy = Some(policy.clone());
            Ok(PolicyBootstrap::Stored)
        }
    }

    fn unprovisioned() -> MockOps {
        MockOps {
            policy: None,
            unseal_calls: 0,
            last_approval_count: 0,
        }
    }

    fn bootstrap_body(n: u8) -> Vec<u8> {
        let (alice, bob, _) = two_of_two();
        json!({
            "n": n,
            "trustees": [
                { "id": "alice", "public_key": hex::encode(alice.verifying_key().to_bytes()) },
                { "id": "bob", "public_key": hex::encode(bob.verifying_key().to_bytes()) },
            ],
        })
        .to_string()
        .into_bytes()
    }

    fn post_policy(ops: &mut MockOps, body: &[u8]) -> HttpReply {
        let mut session = BreakGlassSession::new();
        handle_break_glass(
            ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/policy",
            body,
        )
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
        let opened: serde_json::Value = serde_json::from_str(&reply.body).unwrap();
        assert_eq!(opened["ruleset_hash"], hex::encode(RULESET));
        assert_eq!(opened["requested_by"], "Alice Operator");
        let stored = session.request().expect("session holds the request");
        let context = stored.context.as_ref().expect("context recorded");
        assert_eq!(context.requester_name, "Alice Operator");
        assert_eq!(context.reason_code, "incident-review");
        assert_eq!(context.case_ref.as_deref(), Some("case-9"));

        // The served wire SHOWS what the hash binds — status carries the
        // context plus the ruleset hash and bucket a trustee needs to
        // recompute the request hash (WYSIWYS on the served path).
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "GET",
            "/breakglass/status",
            b"",
        );
        assert_eq!(reply.status, 200);
        let s: serde_json::Value = serde_json::from_str(&reply.body).unwrap();
        assert_eq!(s["requested_by"], "Alice Operator");
        assert_eq!(s["reason"], "incident-review");
        assert_eq!(s["case_ref"], "case-9");
        assert_eq!(s["ruleset_hash"], hex::encode(RULESET));
        assert_eq!(s["time_bucket"]["start_epoch_s"], bucket().start_epoch_s);

        // A case reference WITHOUT the pair would be dropped from the record:
        // refused like a partial pair.
        let mut session3 = BreakGlassSession::new();
        let orphan = json!({
            "envelope": "vault:1",
            "purpose": "incident",
            "case_ref": "case-9"
        })
        .to_string()
        .into_bytes();
        let reply = handle_break_glass(
            &mut ops,
            &mut session3,
            "/out",
            bucket(),
            "POST",
            "/breakglass/request",
            &orphan,
        );
        assert_eq!(reply.status, 400);
        assert!(reply.body.contains("partial_operator_context"));

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
        let mut ops = unprovisioned();
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

    /// The console's one-time setup: the first POST stores the policy (and a
    /// GET reads it back); every later POST is a 409 that names the consented
    /// change flow, and changes nothing.
    #[test]
    fn policy_bootstrap_is_accepted_once_then_409() {
        let mut ops = unprovisioned();
        let reply = post_policy(&mut ops, &bootstrap_body(2));
        assert_eq!(reply.status, 201, "{}", reply.body);
        let v: serde_json::Value = serde_json::from_str(&reply.body).unwrap();
        assert_eq!(v["n"], 2);
        assert_eq!(v["m"], 2);
        assert_eq!(v["trustees"][1]["id"], "bob");
        let stored = ops.policy.clone().expect("policy stored");
        assert_eq!(stored.n, 2);

        let mut session = BreakGlassSession::new();
        let read = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "GET",
            "/breakglass/policy",
            b"",
        );
        assert_eq!(read.status, 200);
        let v: serde_json::Value = serde_json::from_str(&read.body).unwrap();
        assert_eq!(v["trustees"][0]["id"], "alice");

        // A second bootstrap — even a different, valid policy — is refused.
        let again = post_policy(&mut ops, &bootstrap_body(1));
        assert_eq!(again.status, 409);
        assert!(again.body.contains("policy_already_configured"));
        assert!(again.body.contains("policy propose"), "{}", again.body);
        assert_eq!(
            ops.policy.as_ref().unwrap().full_commitment(),
            stored.full_commitment(),
            "a refused bootstrap changes nothing"
        );
    }

    /// A backend that finds a policy the handler did not see (another process
    /// bootstrapped first) reports it, and the reply is the same 409.
    #[test]
    fn policy_bootstrap_race_lost_is_409() {
        struct LateOps(MockOps);
        impl BreakGlassOps for LateOps {
            fn policy(&self) -> Result<Option<QuorumPolicy>> {
                Ok(None)
            }
            fn ruleset_hash(&self) -> [u8; 32] {
                RULESET
            }
            fn authorize_unseal(
                &mut self,
                request: &UnlockRequest,
                approvals: &[Approval],
                now_bucket: TimeBucket,
                output_dir: &str,
            ) -> Result<PathBuf> {
                self.0
                    .authorize_unseal(request, approvals, now_bucket, output_dir)
            }
            fn bootstrap_policy(&mut self, _policy: &QuorumPolicy) -> Result<PolicyBootstrap> {
                Ok(PolicyBootstrap::AlreadyConfigured)
            }
        }
        let mut ops = LateOps(unprovisioned());
        let mut session = BreakGlassSession::new();
        let reply = handle_break_glass(
            &mut ops,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/policy",
            &bootstrap_body(2),
        );
        assert_eq!(reply.status, 409);
        assert!(reply.body.contains("policy_already_configured"));
    }

    /// Validated exactly as `break_glass policy set` validates: threshold,
    /// key length and encoding, duplicate ids, the crypto mode — and nothing
    /// is stored on a refusal.
    #[test]
    fn policy_bootstrap_validates_like_policy_set() {
        let (alice, _bob, _) = two_of_two();
        let alice_hex = hex::encode(alice.verifying_key().to_bytes());
        let cases: Vec<(serde_json::Value, u16)> = vec![
            // threshold above the trustee count
            (
                json!({ "n": 3, "trustees": [{ "id": "alice", "public_key": alice_hex }] }),
                422,
            ),
            // threshold zero
            (
                json!({ "n": 0, "trustees": [{ "id": "alice", "public_key": alice_hex }] }),
                422,
            ),
            // not hex
            (
                json!({ "n": 1, "trustees": [{ "id": "alice", "public_key": "zz" }] }),
                422,
            ),
            // 31 bytes
            (
                json!({ "n": 1, "trustees": [{ "id": "alice", "public_key": &alice_hex[2..] }] }),
                422,
            ),
            // duplicate id
            (
                json!({ "n": 1, "trustees": [
                    { "id": "alice", "public_key": alice_hex },
                    { "id": "alice", "public_key": hex::encode([9u8; 32]) },
                ] }),
                422,
            ),
            // m disagrees with the roster
            (
                json!({ "n": 1, "m": 2, "trustees": [{ "id": "alice", "public_key": alice_hex }] }),
                422,
            ),
            // unknown crypto mode
            (
                json!({ "n": 1, "crypto_mode": "rot13", "trustees": [{ "id": "alice", "public_key": alice_hex }] }),
                422,
            ),
            // unknown field (the server owns everything else)
            (json!({ "n": 1, "trustees": [], "vault": {} }), 400),
            // no trustees
            (json!({ "n": 1, "trustees": [] }), 422),
        ];
        for (body, status) in cases {
            let mut ops = unprovisioned();
            let reply = post_policy(&mut ops, body.to_string().as_bytes());
            assert_eq!(reply.status, status, "{body} -> {}", reply.body);
            assert!(ops.policy.is_none(), "{body} must store nothing");
        }
        let mut ops = unprovisioned();
        assert_eq!(post_policy(&mut ops, b"{not json").status, 400);
        assert!(ops.policy.is_none());
    }

    /// A backend that cannot store a policy (the trait default) says so.
    #[test]
    fn policy_bootstrap_default_backend_refuses() {
        struct ReadOnlyOps;
        impl BreakGlassOps for ReadOnlyOps {
            fn policy(&self) -> Result<Option<QuorumPolicy>> {
                Ok(None)
            }
            fn ruleset_hash(&self) -> [u8; 32] {
                RULESET
            }
            fn authorize_unseal(
                &mut self,
                _request: &UnlockRequest,
                _approvals: &[Approval],
                _now_bucket: TimeBucket,
                _output_dir: &str,
            ) -> Result<PathBuf> {
                Err(anyhow::anyhow!("not provisioned"))
            }
        }
        let mut session = BreakGlassSession::new();
        let reply = handle_break_glass(
            &mut ReadOnlyOps,
            &mut session,
            "/out",
            bucket(),
            "POST",
            "/breakglass/policy",
            &bootstrap_body(2),
        );
        assert_eq!(reply.status, 500);
        assert!(reply.body.contains("policy set"), "{}", reply.body);
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
