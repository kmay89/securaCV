#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root_dir"

cargo test --test compile_fail

fixture_dir="$(mktemp -d)"
fixture_db="$fixture_dir/conformance.db"

cleanup() {
  rm -rf "$fixture_dir"
}
trap cleanup EXIT

# The one fixed, clearly-not-secret seed the fixture kernel opens with
# (examples/conformance_fixture.rs). The DB is SQLCipher-encrypted with a key
# derived from it, so the verifier needs the same seed to read the log —
# which also makes this an end-to-end proof of the seed->key derivation.
fixture_seed="devkey:ci-conformance-fixture-not-a-secret"

cargo run --quiet --example conformance_fixture -- --db "$fixture_db"

cargo run --quiet --bin log_verify -- --db "$fixture_db" \
  --device-key-seed "$fixture_seed"
