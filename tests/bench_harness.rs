//! Performance benchmark harness: it measures, it never promises.
//!
//! Every benchmark here is `#[ignore]`. `cargo test` compiles them, so a
//! signature change fails the build instead of rotting the harness, but never
//! runs them; a run is a deliberate act, in release, on a machine you can name:
//!
//! ```text
//! cargo test --release --test bench_harness -- --ignored --nocapture --test-threads=1
//! ```
//!
//! Each row prints a Markdown table through the crate's own
//! [`witness_kernel::eval::metrics::latency_stats`] (the same summary
//! `detect_eval` prints over its per-frame latencies) and asserts only correctness: the row completed with the
//! expected count, and the verify rows verified. No thresholds, no
//! comparisons, no stored baselines. AGENTS.md rule 4
//! ("no performance claim without a benchmark") has a corollary this file
//! enforces by giving a number nowhere to live: a run's output goes to the
//! terminal or a CI artifact, never into the tree, and
//! `scripts/lint_bench_rows.py` (Repo Lints, unfiltered) is the guard: it reads
//! `ROW_NAMES` below and fails on a measured row, or the append trailer, in
//! any Markdown file. What each row measures, what it deliberately does not,
//! and where the output goes is in `docs/BENCHMARKS.md`.
//!
//! No new dependency: `std::time::Instant`, the eval metrics, and crates the
//! kernel already depends on (`tempfile`, `hex`, `anyhow`). A criterion bench
//! would add a dev-dependency for no product value.

use std::hint::black_box;
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

use tempfile::TempDir;
use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::eval::metrics::latency_stats;
use witness_kernel::eval::LatencyStats;
use witness_kernel::verify_runner::run_full_verify;
use witness_kernel::{
    verify_envelope, CandidateEvent, ContractEnforcer, EventType, ExportOptions, InferenceBackend,
    Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

/// Row names, exactly as the tables print them. `ROW_NAMES` is what the
/// rule-4 guard, `scripts/lint_bench_rows.py`, reads and scans the tree's
/// Markdown for, so keep each name a plain `const ROW_X: &str = "...";` and
/// list it in `ROW_NAMES`: the lint refuses a shape it cannot read.
const ROW_APPEND: &str = "append_event_checked";
const ROW_VERIFY: &str = "run_full_verify";
const ROW_ENVELOPE_BUILD: &str = "build_evidence_envelope_for_api";
const ROW_ENVELOPE_VERIFY: &str = "verify_envelope";
const ROW_CONTRACT: &str = "ContractEnforcer::enforce";
const ROW_BUCKET: &str = "TimeBucket::now_10min";
const ROW_SANDBOX: &str = "execute_sandboxed (no-op module)";
const ROW_NAMES: &[&str] = &[
    ROW_APPEND,
    ROW_VERIFY,
    ROW_ENVELOPE_BUILD,
    ROW_ENVELOPE_VERIFY,
    ROW_CONTRACT,
    ROW_BUCKET,
    ROW_SANDBOX,
];

/// The rows run one at a time even if the test runner is parallel: two
/// benchmarks sharing the cores would measure each other. (`--test-threads=1`
/// in the documented command is the belt; this is the braces.)
static BENCH_LOCK: Mutex<()> = Mutex::new(());

fn serialize() -> MutexGuard<'static, ()> {
    BENCH_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

/// Events per whole-log row, `SECURACV_BENCH_N` (default 10 000).
fn bench_n() -> usize {
    env_usize("SECURACV_BENCH_N", 10_000)
}

/// Passes over a built log or envelope, `SECURACV_BENCH_REPEATS` (default 5).
fn bench_repeats() -> usize {
    env_usize("SECURACV_BENCH_REPEATS", 5)
}

fn env_usize(name: &str, default: usize) -> usize {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .filter(|n| *n > 0)
        .unwrap_or(default)
}

fn ms(d: Duration) -> f64 {
    d.as_secs_f64() * 1_000.0
}

/// Run `f`, push its wall time (ms) onto `samples`, hand back its result. The
/// result is dropped by the caller, so a return value's destructor is never
/// inside the measured window.
fn timed<T>(samples: &mut Vec<f64>, f: impl FnOnce() -> T) -> T {
    let start = Instant::now();
    let out = f();
    samples.push(ms(start.elapsed()));
    out
}

/// One line naming the machine and the profile, so a pasted table can never
/// be mistaken for a claim about some other host. A debug build says so
/// loudly: its numbers describe the optimizer's absence, not the kernel.
fn print_host() {
    let profile = if cfg!(debug_assertions) {
        "DEBUG BUILD (numbers are meaningless; rerun with --release)"
    } else {
        "release"
    };
    println!(
        "host: {} {} | profile: {} | N={} | repeats={}",
        std::env::consts::OS,
        std::env::consts::ARCH,
        profile,
        bench_n(),
        bench_repeats()
    );
}

/// The unit a table's cells print in. The whole-log rows print milliseconds to
/// four decimals. The per-call rows print microseconds to three (nanosecond
/// steps), so a single call's cost is never rounded to the fourth decimal of a
/// millisecond, where a change between two runs could vanish.
#[derive(Clone, Copy)]
enum Unit {
    Millis,
    Micros,
}

impl Unit {
    fn label(self) -> &'static str {
        match self {
            Unit::Millis => "ms",
            Unit::Micros => "µs",
        }
    }

    /// Multiplier from the milliseconds `latency_stats` reports.
    fn per_ms(self) -> f64 {
        match self {
            Unit::Millis => 1.0,
            Unit::Micros => 1_000.0,
        }
    }

    fn decimals(self) -> usize {
        match self {
            Unit::Millis => 4,
            Unit::Micros => 3,
        }
    }
}

fn print_table(title: &str, unit: Unit, rows: &[(&str, LatencyStats)]) {
    for (name, _) in rows {
        assert!(
            ROW_NAMES.contains(name),
            "row {name:?} is not in ROW_NAMES, so scripts/lint_bench_rows.py would not \
             know to look for it: give it a ROW_* constant and list it in ROW_NAMES"
        );
    }
    println!();
    println!("### {title}");
    let (u, k, d) = (unit.label(), unit.per_ms(), unit.decimals());
    println!("| row | n | mean {u} | p50 {u} | p95 {u} | p99 {u} | max {u} |");
    println!("|---|---:|---:|---:|---:|---:|---:|");
    for (name, s) in rows {
        println!(
            "| {name} | {} | {:.d$} | {:.d$} | {:.d$} | {:.d$} | {:.d$} |",
            s.count,
            s.mean_ms * k,
            s.p50_ms * k,
            s.p95_ms * k,
            s.p99_ms * k,
            s.max_ms * k
        );
    }
}

// -------------------- fixtures --------------------

const MODULE: ModuleDescriptor = ModuleDescriptor {
    id: "bench_harness",
    allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
    requested_capabilities: &[],
    supported_backends: &[InferenceBackend::Stub],
};

/// A kernel on a fresh on-disk SQLCipher database in `dir`: the append row
/// pays the same page writes witnessd pays, not an in-memory shortcut.
fn bench_config(dir: &TempDir) -> KernelConfig {
    KernelConfig {
        db_path: dir.path().join("bench.db").to_string_lossy().into_owned(),
        ruleset_id: "ruleset:bench".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:bench"),
        kernel_version: "0.0.0-bench".to_string(),
        retention: Duration::from_secs(3600),
        // MIN_SEED_LENGTH (32) or longer: a short seed is refused at open.
        device_key_seed: "devkey:bench_harness:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

/// A conforming candidate: eight zones, confidence inside `0..=1`, no token.
fn candidate(i: usize, bucket: TimeBucket) -> CandidateEvent {
    CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: format!("zone:bench_{}", i % 8),
        confidence: 0.5 + ((i % 400) as f32) * 0.001,
        correlation_token: None,
        attestation: None,
    }
}

/// Append `n` events through the checked path witnessd uses. Returns the
/// per-call samples and the wall time of the loop; the candidate is built
/// outside the measured window.
fn append_events(kernel: &mut Kernel, cfg: &KernelConfig, n: usize) -> (Vec<f64>, Duration) {
    let bucket = TimeBucket::now_10min().expect("time bucket");
    let mut samples = Vec::with_capacity(n);
    let wall = Instant::now();
    for i in 0..n {
        let cand = candidate(i, bucket);
        timed(&mut samples, || {
            kernel.append_event_checked(
                &MODULE,
                cand,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )
        })
        .expect("append_event_checked");
    }
    (samples, wall.elapsed())
}

// -------------------- rows --------------------

/// Sealed-log append: contract enforcement, hash chaining, Ed25519 signing
/// and the SQLCipher write, per event, N times into one fresh database.
#[test]
#[ignore = "benchmark: run deliberately in release, see docs/BENCHMARKS.md"]
fn kernel_append_throughput() {
    let _serial = serialize();
    print_host();
    let n = bench_n();
    let dir = TempDir::new().expect("tempdir");
    let cfg = bench_config(&dir);
    let mut kernel = Kernel::open(&cfg).expect("open kernel");

    let (samples, wall) = append_events(&mut kernel, &cfg, n);
    let stats = latency_stats(samples);
    assert_eq!(stats.count, n, "every append must have been measured");

    print_table(
        &format!("kernel append throughput (N={n}, fresh SQLCipher database on disk)"),
        Unit::Millis,
        &[(ROW_APPEND, stats)],
    );
    println!(
        "total: {n} events in {:.3} s ({:.0} events/s)",
        wall.as_secs_f64(),
        n as f64 / wall.as_secs_f64()
    );
}

/// Whole-log verification, the walk `log_verify` performs with an
/// out-of-band public key: hash chain, every Ed25519 signature, checkpoint
/// lineage. Measured `repeats` times over one log of N events.
#[test]
#[ignore = "benchmark: run deliberately in release, see docs/BENCHMARKS.md"]
fn log_verification_wall_time() {
    let _serial = serialize();
    print_host();
    let n = bench_n();
    let repeats = bench_repeats();
    let dir = TempDir::new().expect("tempdir");
    let cfg = bench_config(&dir);
    let mut kernel = Kernel::open(&cfg).expect("open kernel");
    append_events(&mut kernel, &cfg, n);
    let key_hex = hex::encode(kernel.device_key_for_verify_only());

    let mut samples = Vec::with_capacity(repeats);
    let mut items_walked = 0usize;
    for _ in 0..repeats {
        items_walked = 0;
        let report = timed(&mut samples, || {
            run_full_verify(
                &kernel.conn,
                Some(&key_hex),
                None,
                SignatureMode::Compat,
                |_| items_walked += 1,
            )
        })
        .expect("verification could be attempted");
        assert!(
            report.chain_valid,
            "a freshly written log must verify: {:?}",
            report.error
        );
        assert!(report.identity_verified, "an out-of-band key was supplied");
        assert_eq!(
            report.events_verified as usize, n,
            "every sealed event was walked"
        );
    }
    let stats = latency_stats(samples);
    assert_eq!(stats.count, repeats);

    print_table(
        &format!(
            "log verification wall time (N={n} sealed events, {items_walked} items walked per pass)"
        ),
        Unit::Millis,
        &[(ROW_VERIFY, stats)],
    );
}

/// Evidence envelope: assembly through the API path (export artifact, export
/// receipt, all four ledgers, whole-envelope digest) and then the offline
/// `verify_envelope` over the last one built. Each pass over one log of N
/// events; each build also seals one export receipt, as the real path does.
#[test]
#[ignore = "benchmark: run deliberately in release, see docs/BENCHMARKS.md"]
fn envelope_build_and_verify() {
    let _serial = serialize();
    print_host();
    let n = bench_n();
    let repeats = bench_repeats();
    let dir = TempDir::new().expect("tempdir");
    let cfg = bench_config(&dir);
    let mut kernel = Kernel::open(&cfg).expect("open kernel");
    append_events(&mut kernel, &cfg, n);

    let mut build = Vec::with_capacity(repeats);
    let mut envelope = None;
    for _ in 0..repeats {
        let built = timed(&mut build, || {
            kernel.build_evidence_envelope_for_api(
                cfg.ruleset_hash,
                ExportOptions::default(),
                &cfg.ruleset_id,
                &cfg.kernel_version,
            )
        })
        .expect("build envelope");
        envelope = Some(built);
    }
    let envelope = envelope.expect("at least one envelope was built");

    let mut verify = Vec::with_capacity(repeats);
    for _ in 0..repeats {
        let report = timed(&mut verify, || {
            verify_envelope(&envelope, SignatureMode::Compat)
        })
        .expect("a freshly built envelope must verify");
        assert_eq!(
            report.sealed_events as usize, n,
            "the envelope carries every sealed event"
        );
    }
    let build = latency_stats(build);
    let verify = latency_stats(verify);
    assert_eq!(build.count, repeats);
    assert_eq!(verify.count, repeats);

    print_table(
        &format!("evidence envelope (N={n} sealed events per envelope)"),
        Unit::Millis,
        &[(ROW_ENVELOPE_BUILD, build), (ROW_ENVELOPE_VERIFY, verify)],
    );
}

/// The per-event cost of the contract itself: `ContractEnforcer::enforce`
/// (bounds, zone allowlist, bucket coarsening, token rule) and the coarse
/// clock read every frame starts with. Neither touches storage and both
/// return values nothing else reads, so `black_box` keeps the optimizer from
/// deleting the call being timed.
#[test]
#[ignore = "benchmark: run deliberately in release, see docs/BENCHMARKS.md"]
fn contract_enforcement_and_time_bucket_cost() {
    let _serial = serialize();
    print_host();
    let n = bench_n();
    let bucket = TimeBucket::now_10min().expect("time bucket");

    let mut enforce = Vec::with_capacity(n);
    for i in 0..n {
        let cand = candidate(i, bucket);
        timed(&mut enforce, || {
            black_box(ContractEnforcer::enforce(black_box(cand)))
        })
        .expect("a conforming candidate passes the contract");
    }

    let mut buckets = Vec::with_capacity(n);
    for _ in 0..n {
        timed(&mut buckets, || black_box(TimeBucket::now_10min())).expect("time bucket");
    }

    let enforce = latency_stats(enforce);
    let buckets = latency_stats(buckets);
    assert_eq!(enforce.count, n);
    assert_eq!(buckets.count, n);

    print_table(
        &format!("contract enforcement and time bucket (N={n} calls each)"),
        Unit::Micros,
        &[(ROW_CONTRACT, enforce), (ROW_BUCKET, buckets)],
    );
}

/// The sandbox boundary's own cost: fork, seccomp, the result pipe, waitpid
/// and the backend state round-trip, with a module whose `process` does
/// nothing. Linux only, by the same `cfg(target_os = "linux")` that
/// `module_runtime::sandbox` itself carries: that is the only platform with a
/// sandbox implementation, and on any other OS `execute_sandboxed` (the call
/// witnessd makes per frame) refuses with "sandbox unavailable", so there is no
/// boundary to time.
#[cfg(target_os = "linux")]
mod sandbox_row {
    use super::*;
    use anyhow::Result;
    use witness_kernel::detect::{BackendRegistry, StubBackend};
    use witness_kernel::{
        BucketKeyManager, CapabilityBoundaryRuntime, FileConfig, FileSource, InferenceView, Module,
    };

    /// A module that declares itself and does nothing, so every microsecond
    /// in the row belongs to the boundary rather than to inference.
    struct NoopModule;

    impl Module for NoopModule {
        fn descriptor(&self) -> ModuleDescriptor {
            MODULE.clone()
        }

        fn process(
            &mut self,
            _view: &InferenceView<'_>,
            _bucket: TimeBucket,
            _token_mgr: &BucketKeyManager,
            _registry: &BackendRegistry,
        ) -> Result<Vec<CandidateEvent>> {
            Ok(Vec::new())
        }
    }

    #[test]
    #[ignore = "benchmark: run deliberately in release, see docs/BENCHMARKS.md"]
    fn sandbox_execute_noop_module_cost() {
        let _serial = serialize();
        print_host();
        let n = bench_n();

        // One synthetic 640x480 frame from the feature-free file source; the
        // same frame is presented every call, so decode is outside the row.
        let mut source = FileSource::new(FileConfig {
            path: "stub://bench".to_string(),
            target_fps: 10,
        })
        .expect("synthetic file source");
        source.connect().expect("connect synthetic source");
        let frame = source.next_frame().expect("one synthetic frame");
        let view = frame.inference_view();

        // The default build's registry shape: a stub backend as the default.
        // One detect() before the loop gives it the state a running witnessd
        // backend holds (the previous frame's hash), so every call below
        // carries the child's state export and the parent's import, exactly
        // as the steady-state loop does. A fresh backend has no state and
        // would leave that half of the boundary off the row.
        let runtime = CapabilityBoundaryRuntime::new();
        let bucket = TimeBucket::now_10min().expect("time bucket");
        let mut token_mgr = BucketKeyManager::new();
        token_mgr.rotate_if_needed(bucket);
        let mut registry = BackendRegistry::new();
        registry.register(StubBackend::new());
        registry
            .set_default("stub")
            .expect("stub backend is the default");
        {
            let backend = registry.default_backend().expect("default backend");
            let mut backend = backend.lock().expect("backend lock");
            backend
                .detect(&[0u8; 64], 8, 8)
                .expect("prime the stub backend");
            assert!(
                backend.export_state().is_some(),
                "a primed backend has state to carry across the boundary"
            );
        }
        let mut module = NoopModule;

        let mut samples = Vec::with_capacity(n);
        for _ in 0..n {
            let candidates = timed(&mut samples, || {
                runtime.execute_sandboxed(&mut module, &view, bucket, &token_mgr, &registry)
            })
            .expect("sandboxed no-op module");
            assert!(candidates.is_empty(), "the no-op module emits nothing");
        }
        let stats = latency_stats(samples);
        assert_eq!(stats.count, n);

        print_table(
            &format!("sandbox boundary (N={n} calls, 640x480 frame, no-op module)"),
            Unit::Millis,
            &[(ROW_SANDBOX, stats)],
        );
    }
}

// -------------------- row names (not ignored) --------------------

#[test]
fn row_names_are_unique() {
    // Every table prints its row name from ROW_NAMES (print_table refuses any
    // other name, so a row printed under an unlisted name fails its own run),
    // and scripts/lint_bench_rows.py reads the same list. Two rows sharing a
    // name would make their tables indistinguishable in a pasted run.
    let mut sorted = ROW_NAMES.to_vec();
    sorted.sort_unstable();
    sorted.dedup();
    assert_eq!(sorted.len(), ROW_NAMES.len(), "row names must be unique");
}
