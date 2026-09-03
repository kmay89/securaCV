#!/usr/bin/env bash
set -euo pipefail
#
# Compatibility wrapper. The TV verifier carry — viewer/verify_core.js, the
# fixtures the website's tv-wall.test.mjs runs it against, and
# tv/vendor/PROVENANCE.txt — now lives in scripts/carry_to_site.py, the one
# tool for every fact the website carries from this repo (the /checkup build
# matrix and the landing page's kernel-status grid ride the same command).
# This name keeps working for anyone who has it in muscle memory or a note.
#
# Usage (from anywhere, with a website checkout):
#   scripts/vendor_tv_verifier.sh [path-to-securacv_website]
#
# Equivalent to:
#   python3 scripts/carry_to_site.py --site <path> --only verifier

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SITE="${1:-${REPO_ROOT}/../securacv_website}"

exec python3 "${SCRIPT_DIR}/carry_to_site.py" --site "${SITE}" --only verifier
