//! Source-level invariants for the BUSY Bar surface.
//!
//! The tests in `src/surface/busybar/` prove the decision layer behaves. These
//! prove something the decision layer cannot: that the code around it has not
//! grown a capability the design forbids.
//!
//! A reviewer can read `SubscribeOnly` and see that the broker handle has no
//! publish method. What a reviewer cannot easily do is notice, two years and
//! forty commits later, that somebody reached around it. These tests notice.
//!
//! They are deliberately blunt — they grep source text. A blunt check that
//! fires on the real regression is worth more than a subtle one nobody
//! maintains, and the failure message says exactly what to do about it.

#![cfg(feature = "surface-busybar")]

use std::path::{Path, PathBuf};

fn repo(rel: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join(rel)
}

fn read(rel: &str) -> String {
    let p = repo(rel);
    std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("reading {}: {e}", p.display()))
}

fn surface_sources() -> Vec<(String, String)> {
    let mut out = vec![(
        "src/bin/busybar_surface.rs".to_string(),
        read("src/bin/busybar_surface.rs"),
    )];
    let dir = repo("src/surface/busybar");
    let mut entries: Vec<PathBuf> = std::fs::read_dir(&dir)
        .expect("src/surface/busybar")
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "rs"))
        .collect();
    entries.sort();
    for p in entries {
        let name = format!(
            "src/surface/busybar/{}",
            p.file_name().unwrap().to_string_lossy()
        );
        out.push((name, std::fs::read_to_string(&p).expect("source")));
    }
    out
}

/// Strip `//` line comments so a rule about *code* is not tripped by prose
/// that explains the rule. Doc comments here talk about publishing at
/// length; that is the point of them.
fn code_only(src: &str) -> String {
    src.lines()
        .map(|l| match l.find("//") {
            Some(i) => &l[..i],
            None => l,
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// Constraint 1, at the source level: this surface cannot write to the
/// broker.
///
/// The daemon wraps its client in `SubscribeOnly`, whose inner handle is
/// private and which exposes `subscribe` and nothing else. If a future change
/// reaches around that — a second client, a direct `.publish(` — this test
/// fires.
#[test]
fn the_surface_has_no_broker_publish_path() {
    for (name, src) in surface_sources() {
        let code = code_only(&src);
        for needle in [".publish(", "try_publish(", "publish_bytes("] {
            assert!(
                !code.contains(needle),
                "{name} contains `{needle}`.\n\
                 The BUSY Bar surface must have no write path to the broker: no control on \
                 the device may affect witnessing, and the way that is guaranteed is that \
                 there is nothing to review. If you genuinely need to publish, that is a \
                 design change — read docs/design/busybar_surface.md section 3 first."
            );
        }
    }
}

/// The surface reads the kernel and never asks it to do anything.
///
/// `POST /verify` does not alter the sealed log, but it does make the kernel
/// work, and a dial detent must not be able to schedule kernel work. `GET` is
/// the only verb.
#[test]
fn the_surface_only_reads_the_kernel() {
    let daemon = code_only(&read("src/bin/busybar_surface.rs"));
    assert!(
        !daemon.contains("/verify"),
        "the surface must not call POST /verify: a control must not be able to schedule \
         kernel work"
    );
    assert!(
        !daemon.contains("/break_glass") && !daemon.contains("break_glass"),
        "the surface must never touch a break-glass path"
    );
    // The one POST it makes is to the bar, which is the glass.
    let posts: Vec<&str> = daemon
        .match_indices(".post(")
        .map(|(i, _)| &daemon[i..(i + 60).min(daemon.len())])
        .collect();
    assert_eq!(
        posts.len(),
        1,
        "the surface should make exactly one kind of POST (the display draw); found {}",
        posts.len()
    );
    assert!(
        posts[0].contains("endpoint"),
        "the one POST should be the display draw: {}",
        posts[0]
    );
}

/// Constraint 3: the surface displays life-safety advisories and never
/// originates one.
///
/// Beacon invariant 1 is that sensors prompt humans and never originate. A
/// desk display is further from a human than a sensor is, so it must not
/// reach the beacon origination path at all — and since it has no publish
/// path (above), it structurally cannot.
#[test]
fn the_surface_cannot_originate_a_beacon() {
    for (name, src) in surface_sources() {
        let code = code_only(&src);
        for needle in ["beacon_originate", "BCN_FLAG", "originate(", "BeaconOrigin"] {
            assert!(
                !code.contains(needle),
                "{name} references `{needle}`. The surface displays advisories; it never \
                 originates them (AGENTS.md, Beacon Channel Invariants, rule 1)."
            );
        }
    }
}

/// Constraint 4: the vendor cloud is refused in code, not in a doc.
#[test]
fn the_local_first_rule_is_enforced_before_a_socket_opens() {
    let whole = read("src/bin/busybar_surface.rs");
    // Compare positions inside `fn main`, not in the import block at the top.
    let daemon = whole
        .split_once("fn main()")
        .expect("the daemon has a main")
        .1;
    let validate_at = daemon
        .find("device::validate_url")
        .expect("the daemon must validate the device URL");
    let socket_at = daemon
        .find("parse_mqtt_endpoint")
        .expect("the daemon must parse the broker endpoint");
    assert!(
        validate_at < socket_at,
        "validate_url must run before anything opens a socket"
    );
}

/// The design doc exists, is on the CI-enforced documentation map, and says
/// out loud that nothing here has been run against real hardware.
///
/// Status honesty is a repo rule, and it is the rule most likely to rot
/// quietly: the day someone benches this, they should have to come here and
/// change the claim on purpose.
#[test]
fn the_docs_say_bench_validation_is_pending() {
    let design = read("docs/design/busybar_surface.md");
    let lowered = design.to_lowercase();
    assert!(
        lowered.contains("bench validation") || lowered.contains("pending bench"),
        "the design doc must state the bench-validation status"
    );
    let index = read("docs/README.md");
    assert!(
        index.contains("design/busybar_surface.md"),
        "docs/README.md is the CI-enforced map; the design doc must be reachable from it"
    );
}

/// The front matrix's variable vocabulary comes from the operator's config
/// and nowhere else.
///
/// `ZoneWord::try_new` is the only constructor. If a `From<String>`, a
/// `FromStr`, or a bare `ZoneWord(` construction appears outside the module
/// that defines it, the closed vocabulary is closed in name only.
#[test]
fn nothing_can_build_a_public_word_from_wire_text() {
    let phrase = read("src/surface/busybar/phrase.rs");
    let code = code_only(&phrase);
    assert!(
        !code.contains("impl From<String> for ZoneWord")
            && !code.contains("impl FromStr for ZoneWord")
            && !code.contains("impl From<&str> for ZoneWord"),
        "ZoneWord must have no conversion from free text; try_new is the only door"
    );

    for (name, src) in surface_sources() {
        if name.ends_with("phrase.rs") {
            continue;
        }
        let code = code_only(&src);
        assert!(
            !code.contains("ZoneWord("),
            "{name} constructs a ZoneWord directly, bypassing the grammar in \
             ZoneWord::try_new. The front matrix's only variable word must come from the \
             operator's validated zone table."
        );
    }
}
