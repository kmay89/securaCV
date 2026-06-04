#!/usr/bin/env bash
#
# verify_published_image.sh — release gate for the pre-built Home Assistant
# add-on image.
#
# A Home Assistant install only works end-to-end if, for EVERY architecture the
# add-on declares (config.yaml `arch:`), the Supervisor can ANONYMOUSLY pull the
# image named by config.yaml `image:`. That requires two things the build
# workflow alone does NOT guarantee:
#
#   1. the `Add-on image` workflow actually pushed that arch's manifest, and
#   2. the resulting GHCR package is PUBLIC (a new package defaults to private;
#      visibility is a one-time, owner-only setting — see the runbook printed at
#      the end of a failing run).
#
# This script checks both, hermetically, with nothing but an anonymous GHCR
# pull token — exactly what a fresh Supervisor on a user's device sees. It is
# the operational counterpart to the build job: green build ≠ installable.
#
# It reads version / image template / arch list straight from config.yaml, so
# it stays in lockstep with the add-on definition (no second source of truth).
#
# Usage:
#   privacy_witness_kernel/verify_published_image.sh            # check :<version> and :latest
#   TAGS="0.5.0" privacy_witness_kernel/verify_published_image.sh
#
# Exit status: 0 iff every declared arch is publicly pullable for every tag.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="$here/config.yaml"
[ -f "$config" ] || { echo "❌ config.yaml not found next to this script ($config)" >&2; exit 2; }

for tool in curl python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "❌ '$tool' is required." >&2; exit 2; }
done

# --- Parse the add-on definition (single source of truth) -------------------
# Use awk (not a grep|sed|tr pipeline): under `set -e`/`pipefail` a grep that
# matches nothing aborts the script before the friendly checks below, whereas
# awk prints an empty string those checks then report cleanly. Quote/space
# stripping is done with Bash parameter expansion (no extra processes).
yaml_scalar() { awk -v key="^$1:" '$0 ~ key { sub(/^[^:]*:[[:space:]]*/, ""); print; exit }' "$config"; }
strip_quotes() { local v="$1"; v="${v//\"/}"; v="${v//\'/}"; printf '%s' "$v"; }

version="$(strip_quotes "$(yaml_scalar version)")"; version="${version//[[:space:]]/}"
image_tmpl="$(strip_quotes "$(yaml_scalar image)")"; image_tmpl="${image_tmpl//[[:space:]]/}"
# arch: is a YAML list of "  - <arch>" lines following the `arch:` key.
mapfile -t arches < <(awk '/^arch:/{f=1;next} f&&/^[[:space:]]*-[[:space:]]*/{gsub(/[[:space:]-]/,"");print;next} f&&/^[^[:space:]-]/{f=0}' "$config")

[ -n "$version" ]    || { echo "❌ could not parse version from config.yaml" >&2; exit 2; }
[ -n "$image_tmpl" ] || { echo "❌ could not parse image: from config.yaml" >&2; exit 2; }
[ "${#arches[@]}" -gt 0 ] || { echo "❌ could not parse arch: list from config.yaml" >&2; exit 2; }
case "$image_tmpl" in
  ghcr.io/*) : ;;
  *) echo "❌ this verifier only understands ghcr.io images (got '$image_tmpl')" >&2; exit 2 ;;
esac

# Tags to check: the pinned version (what the Supervisor resolves) plus latest.
read -r -a tags <<< "${TAGS:-$version latest}"

echo "Add-on:   $(strip_quotes "$(yaml_scalar name)")"
echo "Version:  $version"
echo "Image:    $image_tmpl"
echo "Arches:   ${arches[*]}"
echo "Tags:     ${tags[*]}"
echo "Checking anonymous (Supervisor-equivalent) pullability on GHCR…"
echo

# --- Probe one repo:tag the way an unauthenticated Supervisor would ----------
# Returns the final HTTP status of the manifest GET, retrying transient 5xx/429
# (GHCR briefly returns 503 while a just-pushed manifest is materialising).
probe() {
  local repo="$1" tag="$2" code token attempt
  for attempt in 1 2 3 4 5; do
    token="$(curl -fsS "https://ghcr.io/token?scope=repository:${repo}:pull" 2>/dev/null \
              | python3 -c 'import sys,json;print(json.load(sys.stdin).get("token",""))' 2>/dev/null || true)"
    # `|| echo 000`: under `set -e` a network-level curl failure (DNS, timeout)
    # would otherwise abort the script and skip the retry loop entirely. 000 is
    # treated as transient below so genuine blips get retried, not hard-failed.
    code="$(curl -s -o /dev/null -w '%{http_code}' \
              -H "Authorization: Bearer ${token}" \
              -H 'Accept: application/vnd.oci.image.index.v1+json' \
              -H 'Accept: application/vnd.docker.distribution.manifest.list.v2+json' \
              -H 'Accept: application/vnd.docker.distribution.manifest.v2+json' \
              "https://ghcr.io/v2/${repo}/manifests/${tag}" || echo "000")"
    case "$code" in
      000|429|500|502|503|504) sleep $((attempt * 3)); continue ;;  # transient: back off and retry
      *) break ;;
    esac
  done
  echo "$code"
}

fail=0
for arch in "${arches[@]}"; do
  # config.yaml's image: uses HA's {arch} placeholder; substitute it.
  image="${image_tmpl/\{arch\}/$arch}"
  repo="${image#ghcr.io/}"
  for tag in "${tags[@]}"; do
    code="$(probe "$repo" "$tag")"
    case "$code" in
      200) printf '  ✅ %-9s :%-7s PUBLIC + pullable\n' "$arch" "$tag" ;;
      401|403) printf '  ❌ %-9s :%-7s NOT public (private package or not pushed) — HTTP %s\n' "$arch" "$tag" "$code"; fail=1 ;;
      404) printf '  ❌ %-9s :%-7s tag MISSING (package exists, this tag was not pushed) — HTTP %s\n' "$arch" "$tag" "$code"; fail=1 ;;
      *)   printf '  ❌ %-9s :%-7s unexpected HTTP %s\n' "$arch" "$tag" "$code"; fail=1 ;;
    esac
  done
done

echo
if [ "$fail" -eq 0 ]; then
  echo "✅ All declared arches are publicly pullable. A fresh HA Supervisor can install the add-on."
  exit 0
fi

owner="${image_tmpl#ghcr.io/}"; owner="${owner%%/*}"
cat >&2 <<EOF
❌ At least one architecture is not publicly installable yet.

If the build job is green but an arch shows NOT public / 401 / 403, the GHCR
package is private. A newly published GHCR package is private by default, and
visibility is a ONE-TIME, owner-only setting (it cannot be flipped by a push or
by an automation token — so this gate stays red until a human does it once):

  For each package below, open its settings and set visibility to Public, and
  (recommended) connect it to the repo so it inherits access and shows up there:

    https://github.com/users/${owner}/packages/container/<package>/settings
      → Danger Zone → Change visibility → Public
      → "Connect repository" → kmay89/securaCV

  Packages for this add-on:
EOF
for arch in "${arches[@]}"; do
  image="${image_tmpl/\{arch\}/$arch}"
  echo "    - ${image#ghcr.io/}" >&2
done
cat >&2 <<'EOF'

If instead an arch shows tag MISSING / 404, the build for that arch did not push
that tag — check the latest run of the `Add-on image` workflow (the aarch64 leg
builds under QEMU and is the slow one). Re-run this gate once it finishes.
EOF
exit 1
