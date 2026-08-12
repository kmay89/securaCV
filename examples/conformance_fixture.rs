use anyhow::{anyhow, Result};
use ed25519_dalek::SigningKey;
use witness_kernel::{
    Approval, BreakGlass, BreakGlassToken, CandidateEvent, EventType, ExportOptions,
    InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, QuorumPolicy, TimeBucket,
    TrusteeEntry, TrusteeId, UnlockRequest, ZonePolicy, EXPORT_EVENTS_ENVELOPE_ID,
};

fn main() -> Result<()> {
    let db_path = parse_arg("--db")?;
    let ruleset_id = "ruleset:test".to_string();
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&ruleset_id);
    let mut kernel = Kernel::open(&KernelConfig {
        db_path,
        ruleset_id: ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: std::time::Duration::from_secs(60),
        // A fixture seed, deliberately fixed and clearly not a secret: the
        // kernel now enforces a 32-char minimum on seeds (real deployments
        // generate theirs from urandom), and this example predates the
        // check — the first thing the resurrected conformance gate caught.
        device_key_seed: "devkey:ci-conformance-fixture-not-a-secret".to_string(),
        zone_policy: ZonePolicy::default(),
    })?;

    let module = ModuleDescriptor {
        id: "test-module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket::now(600)?,
        zone_id: "zone:test".to_string(),
        confidence: 0.9,
        correlation_token: None,
        attestation: None,
    };
    kernel.append_event_checked(
        &module,
        cand,
        env!("CARGO_PKG_VERSION"),
        &ruleset_id,
        ruleset_hash,
    )?;

    let mut token = seed_break_glass(&mut kernel, ruleset_hash)?;

    let _artifact = kernel.export_events_authorized(
        ruleset_hash,
        ExportOptions {
            max_events_per_batch: 1,
            jitter_s: 0,
            jitter_step_s: 1,
            ..ExportOptions::default()
        },
        &mut token,
    )?;

    Ok(())
}

fn seed_break_glass(kernel: &mut Kernel, ruleset_hash: [u8; 32]) -> Result<BreakGlassToken> {
    let bucket = TimeBucket::now(600)?;
    let request = UnlockRequest::new(EXPORT_EVENTS_ENVELOPE_ID, ruleset_hash, "audit", bucket)?;
    let signing_key = SigningKey::from_bytes(&[3u8; 32]);
    // Approval::signed produces the DOMAIN-SEPARATED trustee signature the
    // verifier requires; a raw Ed25519 signature over the bare request hash
    // is rejected by design (cross-context replay). The second thing the
    // resurrected conformance gate caught.
    let approval = Approval::signed(
        TrusteeId::new("alice"),
        request.request_hash(),
        &signing_key,
    );
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: signing_key.verifying_key().to_bytes(),
        }],
    )?;
    // The unseal gate re-derives the quorum against the kernel's CONFIGURED
    // policy (Invariant V) — authorizing against a policy the kernel never
    // stored now fails closed. Third catch of the resurrected gate.
    kernel.set_break_glass_policy(&policy)?;
    let (result, receipt) =
        BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result?;
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;
    Ok(token)
}

fn parse_arg(flag: &str) -> Result<String> {
    let mut args = std::env::args();
    while let Some(arg) = args.next() {
        if arg == flag {
            return args
                .next()
                .ok_or_else(|| anyhow!("missing value for {}", flag));
        }
    }
    Err(anyhow!("missing required flag {}", flag))
}
