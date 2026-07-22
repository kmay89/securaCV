//! The Adapter Host: the thin, trusted orchestrator that turns adapter [`Claim`]s into sealed
//! events.
//!
//! The host owns the single [`Kernel`] handle. For every claim an adapter produces, the host:
//! 1. optionally drops it if below a confidence floor (a coarse, host-level filter),
//! 2. deduplicates it within the current time bucket,
//! 3. builds a [`crate::CandidateEvent`] via [`claim_to_candidate`] (sanitizing the zone), and
//! 4. submits it through `Kernel::append_event_checked` — **the only egress to the log.**
//!
//! There is intentionally **no** method on `AdapterHost` that writes an `Event` or
//! `CandidateEvent` to the log bypassing the contract enforcer. The kernel's three gates
//! (allowlist, contract, zone policy) remain the security boundary; the host is convenience
//! plumbing lifted out of the bespoke `frigate_bridge` so every adapter shares it.

use std::collections::{BTreeMap, HashMap};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Result};
use serde::Serialize;

use crate::adapter::contract::{claim_to_candidate, AdapterDescriptor, Claim};
use crate::adapter::registry::AdapterRegistry;
use crate::adapter::SensorAdapter;
use crate::{Event, FailureType, Kernel, TimeBucket};

/// Per-adapter runtime counters, surfaced for observability (see [`AdapterHost::stats_snapshot`]).
///
/// These are operational metrics only — counts and a coarse timestamp, never event content — so
/// exposing them does not widen the privacy surface.
#[derive(Clone, Debug, Default, Serialize)]
pub struct AdapterStats {
    /// Times this adapter has been polled.
    pub polls: u64,
    /// Claims the adapter produced across all polls.
    pub claims_emitted: u64,
    /// Claims that were sealed into the log.
    pub claims_sealed: u64,
    /// Claims dropped by the host (confidence floor or per-bucket dedup).
    pub claims_filtered: u64,
    /// Claims rejected by the kernel's gates (a fail-closed `FailureEvent` was recorded).
    pub claims_rejected: u64,
    /// Poll calls that returned an error.
    pub poll_errors: u64,
    /// Unix epoch (seconds) of the most recently sealed event, or 0 if none.
    pub last_seal_epoch_s: u64,
    /// Latched `GapMissingData` records the host sealed for this adapter's
    /// poll outages (one per outage — see `ADAPTER_OUTAGE_FAILURE_THRESHOLD`).
    pub outage_gaps_sealed: u64,
}

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

/// Consecutive failed polls of a single adapter before the host seals one
/// latched [`FailureType::GapMissingData`] record for the outage.
///
/// This mirrors the ingest path's `IngestSupervisor` (see `bin/witnessd.rs`):
/// "interruption is evidence." Without it a Track-B (adapter) outage — a
/// Frigate broker going away, an MQTT sensor falling silent — leaves no trace
/// in the sealed record, unlike the camera path which already seals a gap.
/// The latch is cleared when the adapter next polls successfully, so exactly
/// one gap is sealed per outage rather than one per failed cycle.
const ADAPTER_OUTAGE_FAILURE_THRESHOLD: u32 = 3;

/// Per-adapter poll-outage state: how many consecutive polls have failed, and
/// whether a gap record has already been sealed for the current outage.
#[derive(Default)]
struct AdapterOutage {
    consecutive_errors: u32,
    gap_sealed: bool,
}

/// Orchestrates registered adapters against a single owned [`Kernel`].
pub struct AdapterHost {
    kernel: Kernel,
    config: AdapterHostConfig,
    registry: AdapterRegistry,
    /// dedup map: key -> bucket start epoch where the claim was last accepted.
    recent: HashMap<String, u64>,
    /// per-adapter runtime counters, keyed by adapter name.
    stats: BTreeMap<String, AdapterStats>,
    /// per-adapter poll-outage state, keyed by adapter name.
    outage: HashMap<String, AdapterOutage>,
}

impl AdapterHost {
    pub fn new(kernel: Kernel, config: AdapterHostConfig) -> Self {
        Self {
            kernel,
            config,
            registry: AdapterRegistry::new(),
            recent: HashMap::new(),
            stats: BTreeMap::new(),
            outage: HashMap::new(),
        }
    }

    /// Snapshot of per-adapter counters, keyed by adapter name. Cheap to clone; intended for a
    /// diagnostic endpoint or periodic logging.
    pub fn stats_snapshot(&self) -> BTreeMap<String, AdapterStats> {
        self.stats.clone()
    }

    /// Update the host-level confidence floor (used for live config reload).
    pub fn set_min_confidence(&mut self, min_confidence: f32) {
        self.config.min_confidence = min_confidence;
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
        // Fail closed: an adapter that requests a forbidden capability (Filesystem/Network) must
        // never have a claim sealed, regardless of how it was registered. Mirrors
        // `CapabilityBoundaryRuntime::validate_descriptor` for modules.
        if let Some(cap) = desc.requested_capabilities.first() {
            return Err(anyhow!(
                "conformance: adapter {} requested forbidden capability {:?}",
                desc.id,
                cap
            ));
        }

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

    /// A successful poll clears any outage latch, so the *next* outage seals a
    /// fresh gap record (exactly one gap per outage).
    fn note_adapter_poll_ok(&mut self, name: &str) {
        if let Some(o) = self.outage.get_mut(name) {
            if o.gap_sealed {
                log::info!(
                    "adapter '{}' recovered after {} consecutive poll error(s)",
                    name,
                    o.consecutive_errors
                );
            }
            o.consecutive_errors = 0;
            o.gap_sealed = false;
        }
    }

    /// Record a failed poll. Once `ADAPTER_OUTAGE_FAILURE_THRESHOLD` consecutive
    /// failures accrue, seal exactly one latched `GapMissingData` record for the
    /// outage. Details carry only the adapter name and the error count — never
    /// URLs, device paths, or payloads, matching the ingest path's rule that
    /// sealed records must not contain network identifiers.
    fn note_adapter_poll_error(&mut self, name: &str) {
        let (consecutive, already_sealed) = {
            let o = self.outage.entry(name.to_string()).or_default();
            o.consecutive_errors = o.consecutive_errors.saturating_add(1);
            (o.consecutive_errors, o.gap_sealed)
        };
        if already_sealed || consecutive < ADAPTER_OUTAGE_FAILURE_THRESHOLD {
            return;
        }

        let bucket = match TimeBucket::now(self.config.bucket_size_secs) {
            Ok(b) => b,
            Err(e) => {
                log::error!(
                    "adapter '{}' outage: could not form time bucket: {}",
                    name,
                    e
                );
                return;
            }
        };
        let details = format!("adapter_stalled adapter={name} consecutive_errors={consecutive}");
        match self.kernel.append_failure_event(
            FailureType::GapMissingData,
            bucket,
            Some(details),
            &self.config.kernel_version,
            &self.config.ruleset_id,
            self.config.ruleset_hash,
        ) {
            Ok(_) => {
                // Latch only after a durable write, so a transient append failure
                // (db lock, disk full, clock skew) retries on the next poll error
                // instead of leaving the outage permanently unrecorded.
                if let Some(o) = self.outage.get_mut(name) {
                    o.gap_sealed = true;
                }
                self.stats
                    .entry(name.to_string())
                    .or_default()
                    .outage_gaps_sealed += 1;
                log::warn!(
                    "sealed GapMissingData record for adapter '{}' outage ({} consecutive poll errors)",
                    name,
                    consecutive
                );
            }
            Err(e) => {
                log::error!("failed to seal adapter-outage record for '{}': {}", name, e);
            }
        }
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
            // A single failing or poisoned adapter must not starve the others this cycle.
            // The poll result is captured inside the guard's scope but handled after it
            // is dropped, so the outage bookkeeping can borrow `self` (the guard borrows
            // only the adapter).
            let (desc, poll_result) = {
                let mut guard = match adapter.lock() {
                    Ok(g) => g,
                    Err(_) => {
                        log::warn!("adapter '{}' lock poisoned; skipping", name);
                        continue;
                    }
                };
                let desc = guard.descriptor();
                (desc, guard.poll())
            };
            let claims = match poll_result {
                Ok(claims) => {
                    self.note_adapter_poll_ok(&name);
                    claims
                }
                Err(e) => {
                    log::warn!("adapter '{}' poll failed: {}; skipping", name, e);
                    self.stats.entry(name.clone()).or_default().poll_errors += 1;
                    // Seal a latched GapMissingData record once the outage crosses
                    // the threshold, so a silent adapter is visible in the log.
                    self.note_adapter_poll_error(&name);
                    continue;
                }
            };
            let stat = self.stats.entry(name.clone()).or_default();
            stat.polls += 1;
            stat.claims_emitted += claims.len() as u64;
            for claim in claims {
                match self.process_claim(desc, &claim) {
                    Ok(Some(_)) => {
                        written += 1;
                        let stat = self.stats.entry(name.clone()).or_default();
                        stat.claims_sealed += 1;
                        stat.last_seal_epoch_s = now_epoch_s();
                    }
                    Ok(None) => self.stats.entry(name.clone()).or_default().claims_filtered += 1,
                    Err(e) => {
                        log::warn!("adapter '{}' claim rejected: {}", name, e);
                        self.stats.entry(name.clone()).or_default().claims_rejected += 1;
                    }
                }
            }
        }
        Ok(written)
    }

    /// Run forever, polling every `poll_interval`. Per-cycle errors are logged, not fatal.
    /// A one-line per-adapter stats summary is logged at INFO roughly once a minute.
    pub fn run_loop(&mut self, poll_interval: Duration) -> Result<()> {
        let log_every = stats_log_interval_cycles(poll_interval);
        let mut cycle: u64 = 0;
        loop {
            if let Err(e) = self.run_once() {
                log::warn!("adapter host poll cycle failed: {}", e);
            }
            cycle += 1;
            if cycle.is_multiple_of(log_every) {
                self.log_stats_summary();
            }
            std::thread::sleep(poll_interval);
        }
    }

    /// Emit a compact per-adapter stats line at INFO.
    pub fn log_stats_summary(&self) {
        for (name, s) in &self.stats {
            log::info!(
                "adapter '{}' stats: polls={} emitted={} sealed={} filtered={} rejected={} poll_errors={} outage_gaps={}",
                name, s.polls, s.claims_emitted, s.claims_sealed, s.claims_filtered, s.claims_rejected, s.poll_errors, s.outage_gaps_sealed
            );
        }
    }
}

fn now_epoch_s() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// How many poll cycles approximate one minute, for the periodic stats log (>= 1).
fn stats_log_interval_cycles(poll_interval: Duration) -> u64 {
    let secs = poll_interval.as_secs().max(1);
    (60 / secs).max(1)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
    use crate::adapter::{LockTolerant, SensorAdapter};
    use crate::{EventType, Kernel, KernelConfig, SealedLogRecord, ZonePolicy};
    use anyhow::anyhow;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};

    static TEST_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
        id: "outage_test_adapter",
        allowed_claim_kinds: &[ClaimKind::AcousticImpulseInZone],
        allowed_event_types: &[EventType::AcousticImpulseInZone],
        requested_capabilities: &[],
    };

    /// A controllable adapter: `fail` toggles whether `poll` errors, and every
    /// poll touches an internal shared mutex through the poison-tolerant path
    /// so a poisoned lock can be exercised end-to-end.
    struct TestAdapter {
        fail: Arc<AtomicBool>,
        state: Arc<Mutex<u32>>,
    }

    impl SensorAdapter for TestAdapter {
        fn name(&self) -> &'static str {
            "outage_test_adapter"
        }
        fn descriptor(&self) -> &'static AdapterDescriptor {
            &TEST_DESCRIPTOR
        }
        fn poll(&mut self) -> Result<Vec<Claim>> {
            // Recover the guard even if a worker poisoned it — the fix under test.
            *self.state.lock_tolerant() += 1;
            if self.fail.load(Ordering::SeqCst) {
                Err(anyhow!("simulated adapter outage"))
            } else {
                Ok(Vec::new())
            }
        }
    }

    fn test_host() -> AdapterHost {
        let hash = KernelConfig::ruleset_hash_from_id("ruleset:outage_test");
        let cfg = KernelConfig {
            db_path: crate::shared_memory_uri(),
            ruleset_id: "ruleset:outage_test".to_string(),
            ruleset_hash: hash,
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:outage_test:0123456789abcdef".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg).expect("open kernel");
        AdapterHost::new(
            kernel,
            AdapterHostConfig {
                bucket_size_secs: 600,
                kernel_version: "0.0.0-test".to_string(),
                ruleset_id: "ruleset:outage_test".to_string(),
                ruleset_hash: hash,
                min_confidence: 0.0,
            },
        )
    }

    fn gap_count(host: &mut AdapterHost) -> usize {
        let hash = host.config.ruleset_hash;
        host.kernel_mut()
            .read_events_ruleset_bound(hash, 1000)
            .expect("read records")
            .into_iter()
            .filter(|r| {
                matches!(r, SealedLogRecord::Failure(f) if f.failure_type == FailureType::GapMissingData)
            })
            .count()
    }

    #[test]
    fn adapter_outage_seals_one_latched_gap_per_outage() {
        let mut host = test_host();
        let fail = Arc::new(AtomicBool::new(true));
        host.register(TestAdapter {
            fail: fail.clone(),
            state: Arc::new(Mutex::new(0)),
        });

        // Below the threshold: failing polls accrue but nothing is sealed yet.
        for _ in 0..(ADAPTER_OUTAGE_FAILURE_THRESHOLD - 1) {
            host.run_once().expect("run_once");
        }
        assert_eq!(gap_count(&mut host), 0, "no gap before the threshold");

        // Crossing the threshold seals exactly one gap.
        host.run_once().expect("run_once");
        assert_eq!(gap_count(&mut host), 1, "one gap at the threshold");

        // Continued failures stay latched — no duplicate gaps for one outage.
        host.run_once().expect("run_once");
        host.run_once().expect("run_once");
        assert_eq!(gap_count(&mut host), 1, "latched: still one gap");

        // Recovery clears the latch without sealing anything.
        fail.store(false, Ordering::SeqCst);
        host.run_once().expect("run_once");
        assert_eq!(gap_count(&mut host), 1, "recovery seals nothing new");

        // A fresh outage seals a second gap.
        fail.store(true, Ordering::SeqCst);
        for _ in 0..ADAPTER_OUTAGE_FAILURE_THRESHOLD {
            host.run_once().expect("run_once");
        }
        assert_eq!(gap_count(&mut host), 2, "a new outage seals a second gap");
    }

    #[test]
    fn poisoned_adapter_lock_does_not_wedge_the_host() {
        let mut host = test_host();
        let state = Arc::new(Mutex::new(0u32));
        host.register(TestAdapter {
            fail: Arc::new(AtomicBool::new(false)),
            state: state.clone(),
        });

        // A worker thread panics while holding the adapter's internal lock,
        // poisoning it — exactly the condition that used to cascade panics.
        let poisoner = state.clone();
        let _ = std::thread::spawn(move || {
            let _g = poisoner.lock().expect("acquire before poisoning");
            panic!("poison the adapter's internal mutex");
        })
        .join();
        assert!(state.lock().is_err(), "internal mutex is poisoned");

        // With `.lock().expect()` this poll would panic and take down the host;
        // with `lock_tolerant()` it recovers and the cycle completes.
        let written = host
            .run_once()
            .expect("run_once must not panic on a poisoned adapter lock");
        assert_eq!(written, 0);
        assert_eq!(
            *state.lock_tolerant(),
            1,
            "poll ran once and advanced the recovered state"
        );
    }

    #[test]
    fn lock_tolerant_recovers_a_poisoned_mutex() {
        let m = Arc::new(Mutex::new(41u32));
        let poisoner = m.clone();
        let _ = std::thread::spawn(move || {
            let _g = poisoner.lock().expect("acquire before poisoning");
            panic!("poison");
        })
        .join();
        assert!(m.lock().is_err(), "mutex is poisoned");
        // Recovers the guard instead of propagating the poison as a panic.
        let mut g = m.lock_tolerant();
        *g += 1;
        assert_eq!(*g, 42);
    }
}
