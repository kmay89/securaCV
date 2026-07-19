#!/usr/bin/env bash
# Configuration-management gate (docs/strategy/12, finding A2): the kernel,
# the HA integration, and the add-on ship as one product and must carry one
# version. This has drifted before (kernel 0.5.0 while the add-on published
# 0.6.0 images built from it). Knight Capital's lesson: the deployed version
# set is part of the software. scripts/bump_version.sh moves all of these in
# lockstep; this lint fails the build when any of them disagree.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || pwd)

cargo_v=$(sed -n 's/^version = "\(.*\)"[[:space:]]*$/\1/p' "${ROOT}/Cargo.toml" | head -n1)
lock_v=$(sed -n '/^name = "witness-kernel"$/{n;s/^version = "\(.*\)"$/\1/p;}' "${ROOT}/Cargo.lock")
manifest_v=$(sed -n 's/.*"version": "\(.*\)".*/\1/p' "${ROOT}/custom_components/securacv/manifest.json" | head -n1)
addon_v=$(sed -n 's/^version: "\(.*\)"[[:space:]]*$/\1/p' "${ROOT}/privacy_witness_kernel/config.yaml" | head -n1)

echo "Cargo.toml:                               ${cargo_v:-<missing>}"
echo "Cargo.lock (witness-kernel):              ${lock_v:-<missing>}"
echo "custom_components/securacv/manifest.json: ${manifest_v:-<missing>}"
echo "privacy_witness_kernel/config.yaml:       ${addon_v:-<missing>}"

if [[ -z "${cargo_v}" || -z "${lock_v}" || -z "${manifest_v}" || -z "${addon_v}" ]]; then
  echo "FAIL: could not extract every component version" >&2
  exit 1
fi

if [[ "${cargo_v}" != "${lock_v}" || "${cargo_v}" != "${manifest_v}" || "${cargo_v}" != "${addon_v}" ]]; then
  echo "FAIL: version drift — run scripts/bump_version.sh <version> so every component moves together" >&2
  exit 1
fi

echo "OK: all components at ${cargo_v}"
