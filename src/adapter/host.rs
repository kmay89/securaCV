//! The Adapter Host: the thin, trusted orchestrator that turns adapter [`Claim`]s into sealed
//! events.
//!
//! The host owns the single [`Kernel`] handle. For every claim an adapter produces, the host:
//! 1. optionally drops it if below a confidence floor (a coarse, host-level filter),
//! 2. deduplicates it within the current time bucket,
//! 3. builds a [`CandidateEvent`] via [`claim_to_candidate`] (sanitizing the zone), and
//! 4. submits it through `Kernel::append_event_checked` — **the only egress to the log.**
//!
//! There is intentionally **no** method on `AdapterHost` that writes an `Event` or
//! `CandidateEvent` to the log bypassing the contract enforcer. The kernel's three gates
//! (allowlist, contract, zone policy) remain the security boundary; the host is convenience
//! plumbing lifted out of the bespoke `frigate_bridge` so every adapter shares it.

use std::collections::HashMap;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Result};

use crate::adapter::contract::{claim_to_candidate, AdapterDescriptor, Claim};
use crate::adapter::registry::AdapterRegistry;
use crate::adapter::SensorAdapter;
use crate::{Event, Kernel, TimeBucket};

/// Configuration for an [`AdapterHost`].
pub struct AdapterHostConfig {
    /// Time-bucket size in seconds. Must be >= 300 (5 min) per `spec/event_contract.md §3`.
    pub bucket_size_secs: u32,
    /// Kernel version string bound into each sealed event.
    pub kernel_version: String,
    /// Ruleset id bound into each sealed event.
    pub ruleset_id: String,
    /// Ruleset hash bound into each sealed event.
    pub ruleset_hash: [u8; 32],
    /// Coarse confidence floor; claims below this are silently dropped before the kernel.
    pub min_confidence: f32,
}

/// Orchestrates registered adapters against a single owned [`Kernel`].
pub struct AdapterHost {
    kernel: Kernel,
    config: AdapterHostConfig,
    registry: AdapterRegistry,
    /// dedup map: key -> bucket start epoch where the claim was last accepted.
    recent: HashMap<String, u64>,
}

impl AdapterHost {
    pub fn new(kernel: Kernel, config: AdapterHostConfig) -> Self {
        Self {
            kernel,
            config,
            registry: AdapterRegistry::new(),
            recent: HashMap::new(),
        }
    }

    /// Register an adapter to be polled by [`run_once`](Self::run_once) / [`run_loop`](Self::run_loop).
    pub fn register<A: SensorAdapter + 'static>(&mut self, adapter: A) {
        self.registry.register(adapter);
    }

    /// Number of registered adapters.
    pub fn adapter_count(&self) -> usize {
        self.registry.len()
    }

    /// Mutable access to the owned kernel (e.g. for exports/verification in tests).
    pub fn kernel_mut(&mut self) -> &mut Kernel {
        &mut self.kernel
    }

    /// Process a single claim from the given adapter descriptor.
    ///
    /// Returns `Ok(Some(event))` if a sealed event was written, `Ok(None)` if the claim was
    /// filtered (below confidence floor or deduplicated), and `Err` if the kernel rejected it
    /// (in which case the kernel has already recorded a fail-closed `FailureEvent`).
    pub fn process_claim(
        &mut self,
        desc: &AdapterDescriptor,
        claim: &Claim,
    ) -> Result<Option<Event>> {
        // Coarse host-level confidence floor (NaN-safe: NaN comparisons are false, so NaN passes
        // here and is rejected authoritatively by the kernel's bounds check).
        if claim.confidence < self.config.min_confidence {
            return Ok(None);
        }

        let bucket = TimeBucket::now(self.config.bucket_size_secs)?;
        let candidate = claim_to_candidate(claim, bucket);

        // Deduplicate within the same bucket (adapter + kind + zone + optional hint).
        let dedup_key = format!(
            "{}:{}:{}:{}",
            desc.id,
            claim.kind.as_str(),
            candidate.zone_id,
            claim.dedup_hint.as_deref().unwrap_or("")
        );
        if let Some(&last_bucket) = self.recent.get(&dedup_key) {
            if last_bucket == bucket.start_epoch_s {
                return Ok(None);
            }
        }

        let module_desc = desc.to_module_descriptor();
        let event = self.kernel.append_event_checked(
            &module_desc,
            candidate,
            &self.config.kernel_version,
            &self.config.ruleset_id,
            self.config.ruleset_hash,
        )?;

        // Only remember accepted claims, and prune stale entries.
        self.recent.insert(dedup_key, bucket.start_epoch_s);
        self.prune_recent();
        Ok(Some(event))
    }

    fn prune_recent(&mut self) {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let window = (self.config.bucket_size_secs as u64).saturating_mul(2);
        self.recent
            .retain(|_, &mut v| now.saturating_sub(v) < window);
    }

    /// Poll every registered adapter once and process all claims they produce.
    /// Returns the number of sealed events written.
    pub fn run_once(&mut self) -> Result<usize> {
        let mut written = 0usize;
        for name in self.registry.list() {
            let adapter = match self.registry.get(&name) {
                Some(a) => a,
                None => continue,
            };
            let (desc, claims) = {
                let mut guard = adapter
                    .lock()
                    .map_err(|_| anyhow!("adapter '{}' lock poisoned", name))?;
                let desc = guard.descriptor();
                let claims = guard.poll()?;
                (desc, claims)
            };
            for claim in claims {
                match self.process_claim(desc, &claim) {
                    Ok(Some(_)) => written += 1,
                    Ok(None) => {}
                    Err(e) => log::warn!("adapter '{}' claim rejected: {}", name, e),
                }
            }
        }
        Ok(written)
    }

    /// Run forever, polling every `poll_interval`. Per-cycle errors are logged, not fatal.
    pub fn run_loop(&mut self, poll_interval: Duration) -> Result<()> {
        loop {
            if let Err(e) = self.run_once() {
                log::warn!("adapter host poll cycle failed: {}", e);
            }
            std::thread::sleep(poll_interval);
        }
    }
}
