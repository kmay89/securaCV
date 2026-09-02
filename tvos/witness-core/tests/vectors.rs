//! The anti-drift gate: prove the TV's chain math is byte-identical to the
//! kernel's, using the kernel's OWN fixtures.
//!
//! `tvos/README.md` says "reuse, don't rebuild — never a second implementation
//! that could disagree with the kernel." The kernel's verifier can't compile
//! for Apple TV (rusqlite/sqlcipher), so this crate re-implements the pinned
//! bytes — exactly as `viewer/verify_core.js` already does for the browser.
//! What makes that safe is this file: it reads
//! `tests/fixtures/envelope/domain_separation_vectors.json` — the same file
//! `src/crypto/signatures.rs` checks itself against — and fails if a single
//! byte differs.
//!
//! If this test ever fails, the TV is wrong (or the fixtures moved). Do not
//! "fix" it by regenerating the vectors from this crate; that would delete the
//! only thing keeping the two in agreement.

use std::path::PathBuf;

use securacv_witness_core::{domain_separated_hash, hash_entry};
use serde_json::Value;

/// The shared fixtures live at the repo root, three levels up from this crate
/// (`tvos/witness-core/tests` → repo root).
fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
}

fn load_vectors() -> Vec<Value> {
    let path = repo_root()
        .join("tests")
        .join("fixtures")
        .join("envelope")
        .join("domain_separation_vectors.json");
    let raw = std::fs::read_to_string(&path).unwrap_or_else(|e| {
        panic!(
            "could not read the shared domain-separation vectors at {}: {e}\n\
             These fixtures are what prove the Apple TV core agrees with the kernel. \
             If they moved, update this path — never delete the check.",
            path.display()
        )
    });
    let doc: Value = serde_json::from_str(&raw).expect("vectors file is not valid JSON");
    doc["vectors"]
        .as_array()
        .expect("vectors file has no `vectors` array")
        .clone()
}

fn hex32(s: &str) -> [u8; 32] {
    let bytes = hex::decode(s).expect("fixture value is not hex");
    bytes.try_into().expect("fixture value is not 32 bytes")
}

#[test]
fn domain_separated_hash_matches_the_kernels_pinned_vectors() {
    let vectors = load_vectors();
    assert!(
        !vectors.is_empty(),
        "the shared vectors file is empty — the anti-drift check would pass vacuously"
    );

    let mut checked = 0usize;
    for v in &vectors {
        let domain = v["domain"].as_str().expect("vector has no domain");
        let entry_hash = hex32(v["entry_hash"].as_str().expect("vector has no entry_hash"));
        let expected = v["signing_hash"]
            .as_str()
            .expect("vector has no signing_hash");

        let got = hex::encode(domain_separated_hash(domain, &entry_hash));
        assert_eq!(
            got, expected,
            "domain_separated_hash disagrees with the kernel for domain {domain:?} \
             and entry_hash {:?} — the Apple TV would verify a different chain than \
             the source of truth",
            v["entry_hash"]
        );
        checked += 1;
    }
    eprintln!("checked {checked} shared domain-separation vectors");
}

#[test]
fn the_sealed_log_entry_domain_string_is_the_one_the_kernel_signs() {
    // Belt and braces: the vectors would still pass if this crate's constant
    // drifted, because the test above takes the domain FROM the fixture. This
    // pins the constant itself.
    let vectors = load_vectors();
    let has_entry_domain = vectors
        .iter()
        .any(|v| v["domain"].as_str() == Some(securacv_witness_core::DOMAIN_SEALED_LOG_ENTRY));
    assert!(
        has_entry_domain,
        "DOMAIN_SEALED_LOG_ENTRY ({:?}) does not appear in the shared vectors — \
         the kernel renamed or re-versioned the sealed-log domain and the TV missed it",
        securacv_witness_core::DOMAIN_SEALED_LOG_ENTRY
    );
}

#[test]
fn hash_entry_is_plain_sha256_of_prev_then_payload() {
    // The kernel's src/log/mod.rs definition, restated as an independent
    // computation so a "clever" refactor here (e.g. length-prefixing the
    // payload) is caught.
    use sha2::{Digest, Sha256};
    let prev = [0xABu8; 32];
    let payload = br#"{"kind":"heartbeat","seq":7}"#;

    let mut expected = Sha256::new();
    expected.update(prev);
    expected.update(payload);
    let expected: [u8; 32] = expected.finalize().into();

    assert_eq!(hash_entry(&prev, payload), expected);
}

#[test]
fn the_domain_length_prefix_is_little_endian_32_bit() {
    // The one byte-layout detail most likely to be got wrong in a rewrite.
    // Recomputed here from first principles rather than from the function.
    use sha2::{Digest, Sha256};
    let domain = securacv_witness_core::DOMAIN_SEALED_LOG_ENTRY;
    let entry_hash = [0x11u8; 32];

    let mut expected = Sha256::new();
    expected.update((domain.len() as u32).to_le_bytes());
    expected.update(domain.as_bytes());
    expected.update(entry_hash);
    let expected: [u8; 32] = expected.finalize().into();

    assert_eq!(domain_separated_hash(domain, &entry_hash), expected);
}

/// The WHOLE document, not just the hash primitive: the kernel serializes a
/// three-entry sealed log under its test seed into
/// `tests/fixtures/envelope/sealed_log_document_vector.json`
/// (`src/api/mod.rs`, `sealed_log_document_matches_the_shared_vector`), and
/// this crate must walk it to the end with its own implementation — same
/// `hash_entry`, same Ed25519 domain, same document shape. Before this test
/// the two sides shared only `domain_separated_hash`; `hash_entry`, the
/// signature path and the field names could drift with nothing going red.
#[test]
fn the_kernels_sealed_log_document_verifies_end_to_end() {
    let path = repo_root()
        .join("tests")
        .join("fixtures")
        .join("envelope")
        .join("sealed_log_document_vector.json");
    let raw = std::fs::read_to_string(&path).unwrap_or_else(|e| {
        panic!(
            "could not read the shared sealed-log vector at {}: {e}\n\
             Regenerate it from the KERNEL (UPDATE_SEALED_LOG_VECTOR=1 cargo test --lib \
             sealed_log_document_matches), never from this crate.",
            path.display()
        )
    });

    let report = securacv_witness_core::verify_json(&raw);
    assert!(
        report.ok,
        "the kernel's own document must verify here: {report:?}"
    );
    let doc: Value = serde_json::from_str(&raw).expect("vector is not valid JSON");
    let entries = doc["entries"].as_array().expect("vector has no entries");
    assert_eq!(entries.len(), 3, "the vector is three planted entries");
    assert_eq!(report.verified, entries.len() as u64);

    // Entry by entry: this crate's hash_entry over the served payload bytes
    // reproduces the kernel's entry_hash, and each prev_hash is the previous
    // entry_hash (the first chains from the all-zero genesis).
    let mut prev = [0u8; 32];
    for (i, entry) in entries.iter().enumerate() {
        let prev_hash = hex32(entry["prev_hash"].as_str().expect("prev_hash"));
        assert_eq!(
            prev_hash, prev,
            "entry {i}: prev_hash is not the previous entry_hash"
        );
        let payload = entry["payload"].as_str().expect("payload");
        let expected = hex32(entry["entry_hash"].as_str().expect("entry_hash"));
        assert_eq!(
            hash_entry(&prev, payload.as_bytes()),
            expected,
            "entry {i}: hash_entry disagrees with the kernel"
        );
        prev = expected;
    }
    assert_eq!(
        report.head,
        hex::encode(prev),
        "the walk's head is the last entry_hash"
    );
}
