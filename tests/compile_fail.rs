//! The programs that must not compile (Invariant I: no raw export path).
//!
//! Each file in `tests/compile_fail/` tries one thing the type system has to
//! refuse: read `RawFrame.data`, reach the frame behind an `InferenceView`,
//! serialize either, or return pixels from a detector. Two things are proven
//! about them, and they are proven on different compilers on purpose:
//!
//! * **On every compiler: each one fails to compile.** That is the invariant.
//! * **On one compiler, the one named in `tests/compile_fail/RUSTC`: it fails
//!   for the stated reason, word for word** (the `.stderr` file beside each
//!   case). That is the evidence that the refusal is the one we meant, and
//!   not, say, a typo in the case.
//!
//! Why not compare the wording everywhere: a `.stderr` file is rustc's own
//! prose, and rustc rewrites its prose. On 2026-11-20 the CI runners' stable
//! moved from 1.99 to 1.100, which draws the "required by a bound in
//! `serialize_field`" note differently, and the Rust workflow went red on
//! main, on two of the five files, with no change to the kernel or to what
//! it refuses. A gate that fails when nothing it guards has changed teaches
//! people to bless whatever the compiler prints; the snapshots therefore
//! name their compiler, and only that compiler is held to them.
//! `.github/workflows/rust.yml` installs it and runs this file on it in one
//! step, whatever the runner's stable is that week.
//!
//! To move the snapshots to a newer compiler: write its version into
//! `tests/compile_fail/RUSTC`, run
//! `TRYBUILD=overwrite cargo +<that version> test --test compile_fail`, and
//! read the diff of every `.stderr` before committing it. On any other
//! compiler `TRYBUILD=overwrite` cannot touch the snapshots (see below).

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const CASES: &str = "tests/compile_fail";

fn manifest_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

/// `1.100.0` out of `rustc 1.100.0 (0ef1bc386 2026-11-09)`: the compiler
/// trybuild's own `cargo` invocations will use (the same `PATH` and
/// `RUSTUP_TOOLCHAIN` this test binary was started with).
fn running_rustc() -> String {
    let rustc = std::env::var_os("RUSTC").unwrap_or_else(|| "rustc".into());
    let out = Command::new(&rustc)
        .arg("--version")
        .output()
        .unwrap_or_else(|e| panic!("could not run {rustc:?} --version: {e}"));
    String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .nth(1)
        .unwrap_or_default()
        .to_string()
}

/// The compiler that wrote the `.stderr` files.
fn snapshot_rustc() -> String {
    let path = manifest_dir().join(CASES).join("RUSTC");
    fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("{}: {e}", path.display()))
        .trim()
        .to_string()
}

/// `1.100` out of `1.100.0`. Diagnostics move with a minor release; a patch
/// release of the same minor is held to the same snapshots.
fn minor(version: &str) -> Option<(u32, u32)> {
    let mut parts = version.split(['.', '-']);
    let major = parts.next()?.parse().ok()?;
    let minor = parts.next()?.parse().ok()?;
    Some((major, minor))
}

fn cases() -> Vec<PathBuf> {
    let dir = manifest_dir().join(CASES);
    let mut found: Vec<PathBuf> = fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("{}: {e}", dir.display()))
        .map(|entry| entry.expect("directory entry").path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "rs"))
        .collect();
    found.sort();
    found
}

#[test]
fn compile_fail_no_raw_export() {
    let (have, want) = (running_rustc(), snapshot_rustc());
    let cases = cases();
    assert!(
        cases.len() >= 5,
        "expected the five refusals in {CASES}, found {}",
        cases.len()
    );
    let t = trybuild::TestCases::new();

    if minor(&have).is_some() && minor(&have) == minor(&want) {
        // The snapshot compiler: the refusal and its wording.
        t.compile_fail(format!("{CASES}/*.rs"));
        return;
    }

    // Any other compiler: the refusal alone. trybuild has no "must not
    // compile, never mind the text" mode, so the cases are copied to a
    // scratch directory with no `.stderr` beside them and run in its
    // `overwrite` mode, which writes the compiler's output next to each case
    // instead of comparing it, and still fails a case that compiles. The
    // writes land in the scratch copies, never in the tree: on this branch
    // `TRYBUILD=overwrite` cannot bless a snapshot with the wrong compiler.
    eprintln!(
        "compile_fail: rustc {have} is not the snapshot compiler ({want}); \
         checking that each case fails to compile, not the wording"
    );
    let scratch = Path::new(env!("CARGO_TARGET_TMPDIR")).join("compile_fail_refusal_only");
    let _ = fs::remove_dir_all(&scratch);
    fs::create_dir_all(&scratch).expect("scratch directory");
    for case in &cases {
        let name = case.file_name().expect("case file name");
        fs::copy(case, scratch.join(name)).expect("copy case");
    }
    std::env::set_var("TRYBUILD", "overwrite");
    t.compile_fail(scratch.join("*.rs"));
}

#[test]
fn every_case_has_its_snapshot_and_the_snapshots_name_their_compiler() {
    let want = snapshot_rustc();
    assert!(
        minor(&want).is_some() && want.split('.').count() == 3,
        "tests/compile_fail/RUSTC must hold one exact version (x.y.z), found {want:?}"
    );
    for case in cases() {
        let stderr = case.with_extension("stderr");
        assert!(
            stderr.is_file(),
            "{} has no snapshot: run the snapshot compiler ({want}) with TRYBUILD=overwrite",
            case.display()
        );
    }
}

#[test]
fn ci_runs_the_wording_check_on_the_snapshot_compiler() {
    // The wording is only ever compared on one compiler, so one CI step has
    // to be that compiler, whatever the runner image ships. The step reads
    // the version from the same file this test does: nothing to keep in step.
    let workflow = manifest_dir().join(".github/workflows/rust.yml");
    let text =
        fs::read_to_string(&workflow).unwrap_or_else(|e| panic!("{}: {e}", workflow.display()));
    for needle in [
        "tests/compile_fail/RUSTC",
        "rustup toolchain install \"$SNAPSHOT_RUSTC\"",
        "cargo \"+$SNAPSHOT_RUSTC\" test --test compile_fail",
    ] {
        assert!(
            text.contains(needle),
            "rust.yml no longer runs the compile-fail wording on the snapshot compiler \
             (missing {needle:?})"
        );
    }
}

#[test]
fn a_minor_is_read_from_a_version() {
    assert_eq!(minor("1.100.0"), Some((1, 100)));
    assert_eq!(minor("1.100.1"), Some((1, 100)));
    assert_eq!(minor("1.101.0-beta.2"), Some((1, 101)));
    assert_eq!(minor("1.99.0"), Some((1, 99)));
    assert_ne!(minor("1.99.0"), minor("1.100.0"));
    assert_eq!(minor(""), None);
    assert_eq!(minor("nightly"), None);
}
