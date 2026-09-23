# espflash sidecars land here at build time (see .github/workflows/desktop-release.yml,
# "Bundle espflash sidecar"): binaries/espflash-<target-triple>, the same pinned,
# sha256-verified espflash the Flasher bundles. Not committed — CI downloads them;
# desktop-lab-check.yml stubs an empty one so `cargo check` can run.
