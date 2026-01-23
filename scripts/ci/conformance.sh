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

cargo run --quiet --example conformance_fixture -- --db "$fixture_db"

cargo run --quiet --bin log_verify -- --db "$fixture_db"
