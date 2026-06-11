#!/usr/bin/env bash
#
# verify_published_image.sh — release gate for the published Docker sidecar.
#
# The quickstart compose files name ghcr.io/kmay89/securacv-sidecar:latest, so
# `docker compose up` only works for users if that package is ANONYMOUSLY
# pullable and the multi-arch index actually contains every platform we claim
# (linux/amd64 + linux/arm64). Neither is guaranteed by a green publish job:
#
#   1. a newly created GHCR package defaults to PRIVATE (one-time, owner-only
#      visibility flip — see the runbook printed by a failing run), and
#   2. a partial multi-arch push leaves the index missing a platform.
#
# This probes GHCR with an anonymous pull token — exactly what a user's Docker
# daemon sees. Operational counterpart of `privacy_witness_kernel/
# verify_published_image.sh`, which guards the HA add-on images the same way.
#
# Usage:
#   docker/sidecar/verify_published_image.sh            # checks :<version> and :latest
#   TAGS="0.6.0" docker/sidecar/verify_published_image.sh
#
# The version tag mirrors the publish job: it is read from the add-on's
# config.yaml (the single version the project ships under).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"
config="$repo_root/privacy_witness_kernel/config.yaml"

for tool in curl python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "❌ '$tool' is required." >&2; exit 2; }
done

repo="kmay89/securacv-sidecar"
platforms=("linux/amd64" "linux/arm64")

version=""
if [ -f "$config" ]; then
  version="$(awk '/^version:/ { sub(/^[^:]*:[[:space:]]*/, ""); gsub(/["'\'' \r]/, ""); print; exit }' "$config")"
fi
read -r -a tags <<< "${TAGS:-${version:+$version }latest}"

echo "Image:     ghcr.io/$repo"
echo "Platforms: ${platforms[*]}"
echo "Tags:      ${tags[*]}"
echo "Checking anonymous (user-equivalent) pullability on GHCR…"
echo

# Fetch a manifest anonymously; print "HTTP_CODE\nBODY". Retries transient
# failures (GHCR briefly 503s while a just-pushed manifest materialises).
fetch_manifest() {
  local tag="$1" token code body attempt
  body=$'\n000'
  for attempt in 1 2 3 4 5; do
    token="$(curl -fsS "https://ghcr.io/token?scope=repository:${repo}:pull" 2>/dev/null \
              | python3 -c 'import sys,json;print(json.load(sys.stdin).get("token",""))' 2>/dev/null || true)"
    if [ -z "$token" ]; then
      # Token endpoint blip: without a bearer the manifest GET would 401 and
      # be misreported as "not public". Treat as transient and retry.
      body=$'\n000'
      sleep $((attempt * 3))
      continue
    fi
    body="$(curl -s -w '\n%{http_code}' \
              -H "Authorization: Bearer ${token}" \
              -H 'Accept: application/vnd.oci.image.index.v1+json' \
              -H 'Accept: application/vnd.docker.distribution.manifest.list.v2+json' \
              -H 'Accept: application/vnd.docker.distribution.manifest.v2+json' \
              "https://ghcr.io/v2/${repo}/manifests/${tag}" || echo "000")"
    code="${body##*$'\n'}"
    case "$code" in
      000|429|500|502|503|504) sleep $((attempt * 3)); continue ;;
      *) break ;;
    esac
  done
  printf '%s' "$body"
}

fail=0
for tag in "${tags[@]}"; do
  response="$(fetch_manifest "$tag")"
  code="${response##*$'\n'}"
  manifest="${response%$'\n'*}"
  case "$code" in
    200) ;;
    401|403)
      printf '❌ :%-7s NOT public (private package or not pushed) — HTTP %s\n' "$tag" "$code"
      fail=1; continue ;;
    404)
      printf '❌ :%-7s tag MISSING (package exists, this tag was not pushed) — HTTP %s\n' "$tag" "$code"
      fail=1; continue ;;
    *)
      printf '❌ :%-7s unexpected HTTP %s\n' "$tag" "$code"
      fail=1; continue ;;
  esac

  # Pullable — now confirm every claimed platform is in the index.
  missing="$(printf '%s' "$manifest" | python3 -c '
import json, sys
want = set(sys.argv[1:])
doc = json.load(sys.stdin)
have = set()
for m in doc.get("manifests", []):
    p = m.get("platform", {})
    if p.get("os") and p.get("architecture"):
        have.add(p["os"] + "/" + p["architecture"])
if not doc.get("manifests"):
    # Single-platform manifest (no index): cannot satisfy a multi-arch claim.
    print(" ".join(sorted(want)))
else:
    print(" ".join(sorted(want - have)))
' "${platforms[@]}")"
  if [ -n "$missing" ]; then
    printf '❌ :%-7s pullable, but the index is missing platform(s): %s\n' "$tag" "$missing"
    fail=1
  else
    printf '✅ :%-7s PUBLIC + pullable, all platforms present (%s)\n' "$tag" "${platforms[*]}"
  fi
done

echo
if [ "$fail" -eq 0 ]; then
  echo "✅ ghcr.io/$repo is publicly installable. 'docker compose up' works for a fresh user."
  exit 0
fi

cat >&2 <<EOF
❌ The sidecar image is not publicly installable yet.

If the publish job is green but a tag shows NOT public / 401 / 403, the GHCR
package is private. A newly published GHCR package is private by default and
visibility is a ONE-TIME, owner-only setting:

    https://github.com/users/kmay89/packages/container/securacv-sidecar/settings
      → Danger Zone → Change visibility → Public
      → "Connect repository" → kmay89/securaCV

If a tag shows MISSING / 404 or a missing platform, the publish job did not
push it — check the latest 'Docker sidecar' workflow run on main.
EOF
exit 1
