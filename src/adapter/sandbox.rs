//! Optional seccomp sandboxing for adapter claim parsing.
//!
//! Adapters parse attacker-controlled bytes (MQTT payloads, webhook bodies, NVR JSON) into
//! [`Claim`]s. That parsing is the realistic attack surface. This helper runs the *pure* parse
//! step inside the same hardened seccomp sandbox the kernel uses for modules
//! (`module_runtime::sandbox::run_in_sandbox`): the work executes in a short-lived forked child
//! that physically cannot open files or sockets — even if the parser is compromised — and the
//! resulting `Vec<Claim>` is serialized back over a pipe.
//!
//! This upgrades the adapter boundary from an *audit* boundary toward a *security* boundary, as
//! anticipated in `spec/sensor_adapter_contract_v0.md §4`. It is **opt-in** per adapter (unlike
//! module sandboxing, which is mandatory) because adapters are out-of-TCB producers and the
//! per-call fork has a cost; operators enable it for adapters whose parsers they don't fully
//! trust. Gated behind the `adapter-sandbox` feature.
//!
//! Only the parse closure runs in the child — channel receipt and kernel writes stay in the
//! trusted parent. The closure must be free of forbidden syscalls (the reference parsers are
//! pure, allocation-only `serde_json`/string work).

use anyhow::Result;

use crate::adapter::contract::Claim;

/// Run a claim-parsing closure inside the seccomp sandbox, returning the parsed claims.
///
/// On non-Linux platforms the underlying sandbox fails closed (returns an error) rather than
/// silently running unsandboxed.
pub fn parse_in_sandbox<F>(parse: F) -> Result<Vec<Claim>>
where
    F: FnOnce() -> Result<Vec<Claim>>,
{
    crate::module_runtime::sandbox::run_in_sandbox(parse)
}

#[cfg(all(test, target_os = "linux"))]
mod tests {
    use super::*;
    use crate::adapter::ClaimKind;

    #[test]
    fn sandboxed_parse_returns_claims() {
        let claims = parse_in_sandbox(|| {
            Ok(vec![Claim::new(
                ClaimKind::AcousticImpulseInZone,
                "garage",
                0.9,
            )])
        })
        .expect("sandboxed parse");
        assert_eq!(claims.len(), 1);
        assert_eq!(claims[0].kind, ClaimKind::AcousticImpulseInZone);
    }

    #[test]
    fn sandboxed_filesystem_access_is_denied() {
        // Proves the denylist applies in adapter context: opening a file inside the sandbox fails,
        // so a compromised parser cannot read/write the host filesystem.
        let result: Result<Vec<Claim>> = parse_in_sandbox(|| {
            std::fs::read("/etc/hostname")
                .map(|_| Vec::new())
                .map_err(|e| anyhow::anyhow!("blocked: {e}"))
        });
        assert!(
            result.is_err(),
            "filesystem access must be denied in sandbox"
        );
    }
}
