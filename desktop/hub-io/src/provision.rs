//! provision — carry the securaCV provisioning bundle onto the boot partition.
//!
//! The bundle (`canary-local/tools/gen_hub_provision_bundle.py`) is the payload a
//! freshly-flashed hub needs to provision itself: the plan, the curated Frigate
//! config, the executor that runs them, and a one-line runner. This module turns
//! that bundle into [`SeedFile`]s under `CONFIG/securacv/`, so they ride the SAME
//! guarded seed machinery as the Wi-Fi keyfile and the account store
//! ([`crate::seed::write_account_seed`] / [`crate::seed::seed_card`]).
//!
//! Single-sourced, not re-implemented. The payload files are embedded VERBATIM
//! from the repo with `include_str!`, and a host test proves each embedded byte
//! matches the SHA-256 the Python manifest pins — so the Rust the flasher ships
//! and the bundle the generator describes can never disagree. The one generated
//! file, the runner, is reproduced here and is pinned by that same cross-check,
//! which is what makes reproducing it safe.
//!
//! Honest scope (docs/design/raspberry_pi_hub_flashing.md): this is the WRITE
//! side — getting the bundle onto `CONFIG/`. Whether HAOS auto-RUNS it at first
//! boot is the on-hardware-pinned hook; until then the bundle is a one-command
//! step, and a bundle HAOS doesn't recognise is simply ignored — never a broken
//! boot.

use crate::account::SeedFile;

/// Where the bundle lands under `CONFIG/` on the boot partition. Matches
/// `bundle_root_on_card` in `canary-local/devices/hub_provision_bundle.json`
/// (which says `CONFIG/securacv`; `CONFIG/` is added by the seed writer).
pub const PROVISION_ROOT: &str = "securacv";

// The bundle's payload, embedded verbatim from the repo. include_str! couples
// this crate's build to these paths ON PURPOSE: move a source and the flasher
// fails to compile rather than silently shipping a stale or empty bundle. The
// cross-check test ties each of these to the Python manifest's SHA-256 pin.
const PLAN: &str = include_str!("../../../canary-local/devices/hub_seed.json");
const FRIGATE_CONFIG: &str = include_str!("../../../homeassistant/frigate/config.yaml");
const EXECUTOR: &str = include_str!("../../../canary-local/tools/hub_seed_apply.py");
const MANIFEST: &str = include_str!("../../../canary-local/devices/hub_provision_bundle.json");

/// The one-line runner, reproduced byte-for-byte from the generator's
/// `runner_script()` (`gen_hub_provision_bundle.py`). If this drifts from the
/// Python original, the cross-check test fails on the runner's pin — so the two
/// can't quietly diverge.
pub fn provision_runner() -> String {
    // Keep in exact lockstep with gen_hub_provision_bundle.py::runner_script().
    concat!(
        "#!/bin/sh\n",
        "# securaCV hub provisioning — one command. See README.md.\n",
        "#   preview (no hub needed):  sh provision.sh --dry-run\n",
        "#   provision (needs SUPERVISOR_TOKEN + http://supervisor):  sh provision.sh\n",
        "set -eu\n",
        "here=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\"\n",
        "exec python3 \"$here/hub_seed_apply.py\" --plan \"$here/hub_seed.json\" ",
        "--assets-root \"$here\" \"$@\"\n",
    )
    .to_string()
}

/// The bundle as [`SeedFile`]s under [`PROVISION_ROOT`], ready to hand to
/// [`crate::seed::write_account_seed`] (which drops them into `CONFIG/`).
///
/// Carries the runnable essentials plus the integrity manifest: the plan, the
/// curated Frigate config, the executor, the runner, and `MANIFEST.json`. The
/// bundle's human `README.md` is deliberately left in the repo/docs rather than
/// on the card — the seed is the runnable payload, not documentation.
pub fn provision_seed_files() -> Vec<SeedFile> {
    let f = |rel: &str, contents: String| SeedFile {
        relative_path: format!("{PROVISION_ROOT}/{rel}"),
        contents,
    };
    vec![
        f("hub_seed.json", PLAN.to_string()),
        f(
            "homeassistant/frigate/config.yaml",
            FRIGATE_CONFIG.to_string(),
        ),
        f("hub_seed_apply.py", EXECUTOR.to_string()),
        f("provision.sh", provision_runner()),
        f("MANIFEST.json", MANIFEST.to_string()),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::seed::write_account_seed;

    fn paths() -> Vec<String> {
        provision_seed_files()
            .into_iter()
            .map(|s| s.relative_path)
            .collect()
    }

    #[test]
    fn every_path_is_namespaced_and_seed_safe() {
        // write_account_seed refuses `..` and absolute paths; ours must pass, and
        // must all sit under securacv/ so they can't collide with HAOS's own
        // CONFIG/ files.
        for p in paths() {
            assert!(p.starts_with("securacv/"), "not namespaced: {p}");
            assert!(!p.contains(".."), "traversal in: {p}");
            assert!(!p.starts_with('/'), "absolute: {p}");
        }
    }

    #[test]
    fn carries_the_runnable_essentials() {
        let mut got = paths();
        got.sort();
        let mut want = vec![
            "securacv/MANIFEST.json",
            "securacv/homeassistant/frigate/config.yaml",
            "securacv/hub_seed.json",
            "securacv/hub_seed_apply.py",
            "securacv/provision.sh",
        ];
        want.sort();
        assert_eq!(got, want);
    }

    #[test]
    fn runner_invokes_the_bundled_executor() {
        let r = provision_runner();
        assert!(r.starts_with("#!/bin/sh\n"));
        assert!(r.contains("hub_seed_apply.py"));
        assert!(r.contains("--plan \"$here/hub_seed.json\""));
        assert!(r.contains("--assets-root \"$here\""));
    }

    /// The load-bearing cross-check: every byte this crate ships must hash to
    /// exactly what the Python manifest pins. This is what lets Rust embed the
    /// payload and reproduce the runner without either drifting from the
    /// generator — a mismatch means the committed manifest is stale or the Rust
    /// runner diverged, and the flasher must not ship that.
    #[test]
    fn embedded_bytes_match_python_manifest_pins() {
        use sha2::Digest;
        let manifest: serde_json::Value = serde_json::from_str(MANIFEST).expect("manifest json");
        let content_for = |bundle_path: &str| -> Option<String> {
            match bundle_path {
                "hub_seed.json" => Some(PLAN.to_string()),
                "homeassistant/frigate/config.yaml" => Some(FRIGATE_CONFIG.to_string()),
                "hub_seed_apply.py" => Some(EXECUTOR.to_string()),
                "provision.sh" => Some(provision_runner()),
                // README is pinned in the manifest but intentionally not carried
                // on the card (docs live in the repo), so it is not cross-checked.
                "README.md" => None,
                other => panic!("unexpected pinned file {other} — teach this test about it"),
            }
        };
        let files = manifest["files"].as_array().expect("files array");
        let mut checked = 0;
        for f in files {
            let bp = f["bundle_path"].as_str().expect("bundle_path");
            let pin = f["sha256"].as_str().expect("sha256");
            if let Some(content) = content_for(bp) {
                let mut h = sha2::Sha256::new();
                h.update(content.as_bytes());
                assert_eq!(
                    crate::sha256_hex(h),
                    pin,
                    "{bp}: embedded bytes != manifest pin"
                );
                checked += 1;
            }
        }
        assert_eq!(
            checked, 4,
            "expected to cross-check plan, config, executor, runner"
        );
    }

    #[test]
    fn flows_through_write_account_seed_into_config() {
        let root = tempfile::tempdir().unwrap();
        let written = write_account_seed(root.path(), &provision_seed_files()).expect("writes");
        assert_eq!(written.len(), 5);
        // The executor and its plan land where the runner expects them.
        let base = root.path().join("CONFIG").join("securacv");
        assert!(base.join("hub_seed_apply.py").exists());
        assert!(base.join("homeassistant/frigate/config.yaml").exists());
        let plan = std::fs::read_to_string(base.join("hub_seed.json")).unwrap();
        assert_eq!(plan, PLAN);
    }
}
