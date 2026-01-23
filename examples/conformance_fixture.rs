use anyhow::{anyhow, Result};
use ed25519_dalek::{Signer, SigningKey};
use witness_kernel::{
    Approval, BreakGlass, CandidateEvent, EventType, ExportOptions, Kernel, KernelConfig,
    ModuleDescriptor, QuorumPolicy, TimeBucket, TrusteeEntry, TrusteeId, UnlockRequest,
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
        device_key_seed: "devkey:ci-conformance".to_string(),
    })?;

    let module = ModuleDescriptor {
        id: "test-module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket::now(600)?,
        zone_id: "zone:test".to_string(),
        confidence: 0.9,
        correlation_token: None,
    };
    kernel.append_event_checked(
        &module,
        cand,
        env!("CARGO_PKG_VERSION"),
        &ruleset_id,
        ruleset_hash,
    )?;

    seed_break_glass(&mut kernel)?;

    let _artifact = kernel.export_events_sequential(
        ruleset_hash,
        ExportOptions {
            max_events_per_batch: 1,
            jitter_s: 0,
            jitter_step_s: 1,
        },
    )?;

    Ok(())
}

fn seed_break_glass(kernel: &mut Kernel) -> Result<()> {
    let bucket = TimeBucket::now(600)?;
    let request = UnlockRequest::new("vault:1", [9u8; 32], "audit", bucket)?;
    let signing_key = SigningKey::from_bytes(&[3u8; 32]);
    let signature = signing_key.sign(&request.request_hash());
    let approval = Approval::new(
        TrusteeId::new("alice"),
        request.request_hash(),
        signature.to_vec(),
    );
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: signing_key.verifying_key().to_bytes(),
        }],
    )?;
    let (_, receipt) = BreakGlass::authorize(&policy, &request, &[approval.clone()], bucket);
    kernel.append_break_glass_receipt(&receipt, &[approval])?;
    Ok(())
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
