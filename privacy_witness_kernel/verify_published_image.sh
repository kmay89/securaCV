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
# It reads version / image template / arch list straight from config.yaml, so
# it stays in lockstep with the add-on definition (no second source of truth).
#
# TWO PROBES, BECAUSE ONE CANNOT TELL THE FAILURES APART
# GHCR answers an anonymous manifest GET for a PRIVATE package with the same
# 404 it gives for a package or tag that was never pushed — it will not even
# confirm that a private package exists. The first version of this gate read
# that 404 as "tag MISSING (package exists, this tag was not pushed)", and said
# so, wrongly, on a run that had pushed both arches fine and merely never had
# its visibility flipped. So each repo:tag is probed twice:
#
#   ANONYMOUS      what a fresh Supervisor sees. 200 = public and pullable.
#   AUTHENTICATED  the same GET with a registry token exchanged for a GitHub
#                  token (GHCR_PROBE_TOKEN — in CI, the workflow's own
#                  GITHUB_TOKEN). Optional. With it: authenticated 200 while
#                  the anonymous GET was not 200 = PRIVATE (pushed, visibility
#                  never flipped); authenticated 404 = NOT PUSHED. Without it
#                  the script cannot tell those two apart and says so.
#
# Usage:
#   privacy_witness_kernel/verify_published_image.sh            # :<version> and :latest
#   TAGS="0.5.0" privacy_witness_kernel/verify_published_image.sh
#   GHCR_PROBE_TOKEN=<GitHub token with packages:read> GHCR_PROBE_USER=<its login> \
#     privacy_witness_kernel/verify_published_image.sh
#
# Exit status (distinct on purpose — the workflow annotates each case):
#   0  every declared arch is publicly pullable for every tag
#   1  not installable, cause undetermined: no GHCR_PROBE_TOKEN to cross-probe
#      with, the token could not read the package, or an HTTP status this
#      script does not classify
#   2  usage / configuration error (config.yaml unparseable, a tool missing)
#   3  PRIVATE — at least one arch:tag is pushed but not public
#   4  NOT PUSHED — at least one arch:tag is absent even to the authenticated probe
#   When cases mix, the most actionable wins: 4 over 3 over 1.
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

probe_token="${GHCR_PROBE_TOKEN:-}"
probe_user="${GHCR_PROBE_USER:-token}"

echo "Add-on:   $(strip_quotes "$(yaml_scalar name)")"
echo "Version:  $version"
echo "Image:    $image_tmpl"
echo "Arches:   ${arches[*]}"
echo "Tags:     ${tags[*]}"
if [ -n "$probe_token" ]; then
  echo "Probe:    anonymous (Supervisor view) + authenticated cross-probe (tells PRIVATE from NOT PUSHED)"
else
  echo "Probe:    anonymous only — set GHCR_PROBE_TOKEN to tell a private package from an unpushed tag"
fi
echo "Checking anonymous (Supervisor-equivalent) pullability on GHCR…"
echo

# --- Registry token for one repo: anonymous, or exchanged for a GitHub token --
# GHCR hands out a pull-scoped registry token at /token; anonymous callers get
# one too (it just cannot see private packages). In `auth` mode the GitHub
# token goes in as HTTP basic auth — through a curl config on stdin, never on
# the command line, so it cannot show up in a process listing.
registry_token() {
  local repo="$1" mode="$2" url json
  url="https://ghcr.io/token?service=ghcr.io&scope=repository:${repo}:pull"
  if [ "$mode" = auth ]; then
    json="$(printf 'user = "%s:%s"\n' "$probe_user" "$probe_token" \
              | curl -fsS -K - "$url" 2>/dev/null || true)"
  else
    json="$(curl -fsS "$url" 2>/dev/null || true)"
  fi
  printf '%s' "$json" | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("token", ""))
except Exception:
    print("")'
}

# --- Probe one repo:tag ------------------------------------------------------
# Prints the final HTTP status of the manifest GET, retrying transient 5xx/429
# (GHCR briefly returns 503 while a just-pushed manifest is materializing).
# In `auth` mode prints `notoken` when the token exchange itself failed, so a
# failed exchange can never be misread as a 404 (= "not pushed").
probe() {
  local repo="$1" tag="$2" mode="$3" code token attempt
  code=000
  for attempt in 1 2 3 4 5; do
    token="$(registry_token "$repo" "$mode")"
    if [ "$mode" = auth ] && [ -z "$token" ]; then
      # One retry covers a blip on the token endpoint; an invalid token fails
      # the same way every time, so do not burn the full back-off on it.
      code=notoken
      [ "$attempt" -ge 2 ] && break
      sleep 3; continue
    fi
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

private=0; notpushed=0; undetermined=0
for arch in "${arches[@]}"; do
  # config.yaml's image: uses HA's {arch} placeholder; substitute it.
  image="${image_tmpl/\{arch\}/$arch}"
  repo="${image#ghcr.io/}"
  for tag in "${tags[@]}"; do
    anon="$(probe "$repo" "$tag" anon)"
    if [ "$anon" = 200 ]; then
      printf '  ✅ %-9s :%-7s PUBLIC + pullable\n' "$arch" "$tag"
      continue
    fi
    if [ -z "$probe_token" ]; then
      printf '  ❌ %-9s :%-7s NOT anonymously pullable (HTTP %s) — private OR never pushed; without GHCR_PROBE_TOKEN this script cannot tell which\n' "$arch" "$tag" "$anon"
      undetermined=1
      continue
    fi
    auth="$(probe "$repo" "$tag" auth)"
    case "$auth" in
      200)     printf '  ❌ %-9s :%-7s PRIVATE — pushed (authenticated pull works) but the anonymous GET is HTTP %s\n' "$arch" "$tag" "$anon"; private=1 ;;
      404)     printf '  ❌ %-9s :%-7s NOT PUSHED — HTTP 404 even to the authenticated probe\n' "$arch" "$tag"; notpushed=1 ;;
      401|403) printf '  ❌ %-9s :%-7s probe token cannot read it (HTTP %s authenticated, %s anonymous) — the token lacks packages:read, or the package is not connected to this repository\n' "$arch" "$tag" "$auth" "$anon"; undetermined=1 ;;
      notoken) printf '  ❌ %-9s :%-7s could not exchange GHCR_PROBE_TOKEN for a registry token (anonymous GET was HTTP %s)\n' "$arch" "$tag" "$anon"; undetermined=1 ;;
      *)       printf '  ❌ %-9s :%-7s unexpected HTTP %s (authenticated) / %s (anonymous)\n' "$arch" "$tag" "$auth" "$anon"; undetermined=1 ;;
    esac
  done
done

echo
if [ "$private$notpushed$undetermined" = 000 ]; then
  echo "✅ All declared arches are publicly pullable. A fresh HA Supervisor can install the add-on."
  exit 0
fi

owner="${image_tmpl#ghcr.io/}"; owner="${owner%%/*}"
echo "❌ At least one architecture is not publicly installable yet." >&2

if [ "$private" = 1 ]; then
  cat >&2 <<EOF

PRIVATE: the image is pushed; the package is not public. A newly published GHCR
package is private by default, and visibility is a ONE-TIME, owner-only setting
(it cannot be flipped by a push or by an automation token — so this gate stays
red until a human does it once):

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
fi

if [ "$notpushed" = 1 ]; then
  cat >&2 <<'EOF'

NOT PUSHED: the build for that arch did not publish that tag. Check this run's
`build` legs in the `Add-on image` workflow — one native runner per arch; a leg
that failed or was skipped leaves its arch without the tag. Re-run this gate
once the push has landed.
EOF
fi

if [ "$undetermined" = 1 ]; then
  cat >&2 <<'EOF'

UNDETERMINED: an arch is not anonymously pullable and the script could not
establish why. Without GHCR_PROBE_TOKEN a private package and an unpushed tag
look identical from outside (both 404) — run again with a GitHub token that has
packages:read (in CI the workflow passes its own GITHUB_TOKEN). If the token
could not read the package either, connect the package to the repository
(package settings → "Connect repository") or use a token whose owner can see it.
EOF
fi

# Most actionable first: an unpushed tag is a build problem, a private package
# is a one-time human action, undetermined needs a better probe.
if [ "$notpushed" = 1 ]; then exit 4; fi
if [ "$private" = 1 ]; then exit 3; fi
exit 1
