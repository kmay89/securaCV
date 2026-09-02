#!/usr/bin/env bash
set -euo pipefail
# SecuraCV universal installer — one command, narrated, idempotent.
#
# Usage (Home Assistant OS Terminal/SSH add-on, or any Docker host):
#   curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash
#
# What it does depends on where it runs:
#   * Home Assistant OS (the 'ha' CLI answers): full unattended provisioning —
#     Mosquitto, the 'canary' broker login, the HA MQTT config entry, Frigate
#     with its curated config, the Privacy Witness Kernel app in frigate mode,
#     the SecuraCV integration + its config entry, blueprints and dashboards.
#   * A Docker host (no 'ha', docker present): the SecuraCV sidecar next to
#     your existing Frigate + broker (bundling a broker if you have none).
#   * Neither: it explains the two paths and exits.
#
# Re-running is safe: every step checks what is already done and skips it,
# saying so. Nothing here ever prints a password or writes a device key —
# the kernel app generates and keeps its own signing key.
#
# Environment overrides (all optional):
#   SECURACV_BRANCH       git ref (branch or tag) to install from. Default:
#                         the latest GitHub release tag, so a re-run a month
#                         later installs the same reviewed tree and not
#                         whatever main holds that minute; falls back to
#                         main, saying so, when the release lookup fails.
#   SECURACV_TARBALL_SHA256
#                         expected SHA-256 of the downloaded archive; when
#                         set, a mismatch aborts before anything is extracted
#                         (GitHub source tarballs are not byte-stable across
#                         time, so pin the hash you verified, not one from a
#                         list — there is none to publish)
#   SECURACV_TARBALL      path to a local .tar.gz of the repo instead of
#                         downloading (used by the test suite)
#   SECURACV_WITH         space-separated optional plan features, e.g.
#                         "pihole display" (passed to the provisioner)
#   SECURACV_PROVISIONER  auto|python|bash — which provisioning engine to use
#                         on Home Assistant OS (default: auto)
#   HA_CONFIG_DIR         Home Assistant config dir (default: /config)
#   ADDON_CONFIGS_DIR     app config base dir (default: /addon_configs)
#   SECURACV_HOME         install dir on the Docker path (default: ./securacv)
#   SUPERVISOR_URL        Supervisor API base (default: http://supervisor)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
GITHUB_ORG="kmay89"
GITHUB_REPO="securaCV"
# Resolved in fetch_source (a release lookup needs the network and the
# logging helpers); SECURACV_BRANCH set by the operator always wins.
SECURACV_BRANCH="${SECURACV_BRANCH:-}"
TARBALL_URL=""

HA_CONFIG_DIR="${HA_CONFIG_DIR:-/config}"
ADDON_CONFIGS_DIR="${ADDON_CONFIGS_DIR:-/addon_configs}"
# Exported: the vendored provisioner and ha_onboard.py read it from the
# environment, and all three must agree on the same base URL.
export SUPERVISOR_URL="${SUPERVISOR_URL:-http://supervisor}"

MOSQUITTO_SLUG="core_mosquitto"
FRIGATE_REPO="https://github.com/blakeblackshear/frigate-hass-addons"
# Supervisor app slugs are "<repo-hash>_<addon-slug>" where repo-hash is
# sha1(lowercased repo URL)[:8] — ccab4aaf for blakeblackshear's repo,
# d0491a67 for this one.
FRIGATE_SLUG="ccab4aaf_frigate"
SECURACV_ADDON_REPO="https://github.com/kmay89/securaCV"
SECURACV_ADDON_SLUG="d0491a67_privacy_witness_kernel"

# ---------------------------------------------------------------------------
# Terminal colors (gracefully degraded if not a tty)
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
  RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
  BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; BLUE=''; BOLD=''; NC=''
fi

# ---------------------------------------------------------------------------
# Logging helpers. Steps are numbered ("Step 3/8 — …") and each states what
# it does and why in one line, so a person watching the terminal can answer
# "what is this doing to my machine?" at every moment.
# ---------------------------------------------------------------------------
STEP_NO=0
STEP_TOTAL=0

log_info()  { printf '%b[SecuraCV]%b %s\n' "$BLUE" "$NC" "$*"; }
log_ok()    { printf '%b  ✓%b %s\n' "$GREEN" "$NC" "$*"; }
log_warn()  { printf '%b  !%b %s\n' "$YELLOW" "$NC" "$*"; }
log_error() { printf '%b  ✗%b %s\n' "$RED" "$NC" "$*" >&2; }
log_step()  {
  STEP_NO=$((STEP_NO + 1))
  printf '\n%bStep %d/%d — %s%b\n' "$BOLD" "$STEP_NO" "$STEP_TOTAL" "$*" "$NC"
}

die() { log_error "$*"; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# Every remaining manual step lands here so the final summary can state them
# honestly and precisely — or say "nothing left to do" and mean it.
LEFTOVERS=()
add_leftover() { LEFTOVERS+=("$*"); }

# Every 'ha' CLI call goes through this one wrapper (tests shim the binary on
# PATH; a single choke point also keeps the call sites greppable).
ha_cmd() { ha "$@"; }

# ---------------------------------------------------------------------------
# Workspace: one temp dir for the whole run, cleaned on exit.
# ---------------------------------------------------------------------------
WORKDIR=""
SRC_ROOT=""
ONBOARD_PY=""

cleanup() { [ -n "$WORKDIR" ] && rm -rf "$WORKDIR"; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Shared: fetch the repo source archive ONCE and extract all of it.
# Both paths (HA OS and Docker) consume files from it — the curated Frigate
# config, the provisioning plan + executor, the integration, blueprints,
# dashboards, compose files — so one download serves everything.
# ---------------------------------------------------------------------------
fetch_source() {
  WORKDIR="$(mktemp -d)"
  local tarball="${WORKDIR}/securacv.tar.gz"

  if [ -n "${SECURACV_TARBALL:-}" ]; then
    [ -f "$SECURACV_TARBALL" ] || die "SECURACV_TARBALL points at '$SECURACV_TARBALL', which does not exist."
    log_info "Using local source archive ${SECURACV_TARBALL} (SECURACV_TARBALL override)."
    cp "$SECURACV_TARBALL" "$tarball"
  else
    # Which tree to install. A moving branch means two people running the
    # same one-liner an hour apart can get different code; the latest
    # release tag is a tree somebody cut on purpose. main stays available
    # (SECURACV_BRANCH=main) and is the fallback when the lookup fails —
    # offline API, rate limit, a repo with no releases yet — said out loud.
    if [ -z "$SECURACV_BRANCH" ]; then
      local latest=""
      latest="$(curl -fsSL --max-time 15 \
        "https://api.github.com/repos/${GITHUB_ORG}/${GITHUB_REPO}/releases/latest" 2>/dev/null \
        | grep -o '"tag_name"[[:space:]]*:[[:space:]]*"[^"]*"' \
        | sed 's/.*"\([^"]*\)"$/\1/' | head -n1 || true)"
      if [ -n "$latest" ]; then
        SECURACV_BRANCH="$latest"
        log_info "Installing the latest release, ${SECURACV_BRANCH} (set SECURACV_BRANCH to pick another ref)."
      else
        SECURACV_BRANCH="main"
        log_warn "Could not resolve the latest release tag from GitHub; installing from branch main instead."
      fi
    fi
    TARBALL_URL="https://api.github.com/repos/${GITHUB_ORG}/${GITHUB_REPO}/tarball/${SECURACV_BRANCH}"
    log_info "Downloading ${GITHUB_ORG}/${GITHUB_REPO} (ref ${SECURACV_BRANCH}) — one archive covers every file this install needs."
    # Temp-file-then-move: a partial download can never masquerade as a
    # complete archive.
    local part="${tarball}.part"
    if ! curl -fsSL "$TARBALL_URL" -o "$part"; then
      die "Download failed (${TARBALL_URL}). Check network connectivity to github.com and that ref '${SECURACV_BRANCH}' exists. Nothing on this machine was changed."
    fi
    mv "$part" "$tarball"
    log_ok "Downloaded source archive"
  fi

  # An operator who verified an archive can pin its hash; a mismatch stops
  # here, before extraction, with nothing on the machine changed.
  if [ -n "${SECURACV_TARBALL_SHA256:-}" ]; then
    local got=""
    if have sha256sum; then
      got="$(sha256sum "$tarball" | cut -d' ' -f1)"
    elif have shasum; then
      got="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
    else
      die "SECURACV_TARBALL_SHA256 is set but neither sha256sum nor shasum is available to check it."
    fi
    if [ "$got" != "$SECURACV_TARBALL_SHA256" ]; then
      die "Source archive SHA-256 mismatch: expected ${SECURACV_TARBALL_SHA256}, got ${got}. Refusing to extract it."
    fi
    log_ok "Source archive SHA-256 matches SECURACV_TARBALL_SHA256"
  fi

  mkdir -p "${WORKDIR}/src"
  if ! tar -xzf "$tarball" -C "${WORKDIR}/src"; then
    die "Extract failed: the archive downloaded but is not a valid .tar.gz (truncated download, or a GitHub error page?). Re-run to retry."
  fi

  # GitHub tarballs wrap everything in one top-level "<org>-<repo>-<sha>/"
  # directory; a plain archive may not. Handle both.
  local entries=()
  local e
  for e in "${WORKDIR}/src"/* ; do
    [ -e "$e" ] && entries+=("$e")
  done
  if [ "${#entries[@]}" -eq 0 ]; then
    die "Extract produced no files — the archive appears to be empty."
  fi
  if [ "${#entries[@]}" -eq 1 ] && [ -d "${entries[0]}" ]; then
    SRC_ROOT="${entries[0]}"
  else
    SRC_ROOT="${WORKDIR}/src"
  fi
  log_ok "Source extracted to a temporary workspace"

  # The onboarding helper (core-API work) ships in the same archive; when the
  # script runs from a checkout, the sibling copy works too.
  local script_dir=""
  if script_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" 2>/dev/null && pwd)"; then
    :
  else
    script_dir=""
  fi
  if [ -f "${SRC_ROOT}/scripts/ha_onboard.py" ]; then
    ONBOARD_PY="${SRC_ROOT}/scripts/ha_onboard.py"
  elif [ -n "$script_dir" ] && [ -f "${script_dir}/ha_onboard.py" ]; then
    ONBOARD_PY="${script_dir}/ha_onboard.py"
  else
    ONBOARD_PY=""
  fi
}

# ===========================================================================
# PATH 1 — Home Assistant OS
# ===========================================================================

TOKEN_OK=true
KERNEL_ARCH_OK=true

ha_check_prereqs() {
  log_step "Check prerequisites — confirm this terminal can actually drive the install"

  have curl || die "'curl' is required but not found. Install it and re-run."
  log_ok "curl available"

  if [ -z "${SUPERVISOR_TOKEN:-}" ]; then
    TOKEN_OK=false
    log_warn "SUPERVISOR_TOKEN is not set in this shell — repository registration and core-API steps will be skipped, and the rest continues degraded."
    log_warn "(The Terminal/SSH add-on with Protection mode off provides it; re-running from there completes what this run cannot.)"
  else
    log_ok "Supervisor token present — the Supervisor and core APIs are reachable"
  fi

  local arch
  arch="$(uname -m)"
  if [ "$arch" = "armv7l" ]; then
    KERNEL_ARCH_OK=false
    log_warn "CPU is armv7 (32-bit). The witness kernel app ships amd64/aarch64 builds only, so that step will be skipped on this machine; everything else proceeds."
  else
    log_ok "Architecture: ${arch}"
  fi
}

# --- provisioning -----------------------------------------------------------

ha_provision() {
  log_step "Provision the hub — broker, broker login, MQTT entry, Frigate + config, witness kernel: the pieces that turn camera detections into signed claims"

  if [ "$TOKEN_OK" != true ]; then
    log_warn "No Supervisor token, so the plan-driven provisioner cannot run — using the degraded fallback."
    ha_provision_fallback
    return 0
  fi

  local engine="${SECURACV_PROVISIONER:-auto}"
  if [ "$engine" = "bash" ]; then
    log_info "SECURACV_PROVISIONER=bash — using the fallback provisioner by request."
    ha_provision_fallback
    return 0
  fi

  if ! have python3; then
    log_info "python3 is not installed here, and the provisioning plan is driven by a small Python tool — trying 'apk add --no-cache python3'…"
    if have apk && apk add --no-cache python3 >/dev/null 2>&1; then
      log_ok "python3 installed"
    else
      log_warn "Could not install python3 — falling back to the built-in bash provisioner (it covers most, not all, of the plan)."
      ha_provision_fallback
      return 0
    fi
  fi

  local -a with_flags=()
  if [ -n "${SECURACV_WITH:-}" ]; then
    local -a features=()
    read -r -a features <<< "${SECURACV_WITH}"
    local f
    for f in "${features[@]}"; do
      with_flags+=(--with "$f")
    done
  fi

  log_info "Running the provisioning plan (canary-local/tools/hub_seed_apply.py) — it narrates each action and why it exists as it goes."
  if python3 "${SRC_ROOT}/canary-local/tools/hub_seed_apply.py" \
      --plan "${SRC_ROOT}/canary-local/devices/hub_seed.json" \
      --assets-root "${SRC_ROOT}" \
      "${with_flags[@]}"; then
    log_ok "Provisioning plan applied"
  else
    log_warn "The provisioning plan stopped early (its own output above says exactly where). It is idempotent — fix the cause and re-run this installer; finished actions will be skipped."
    add_leftover "Re-run this installer after fixing the provisioning failure reported above (re-running is safe; finished steps are skipped)."
  fi
}

addon_state() {
  local s
  s="$(ha_cmd addons info "$1" 2>/dev/null | grep -E '^state:' | awk '{print $2}' || true)"
  printf '%s' "${s:-not_installed}"
}

fb_register_repositories() {
  if [ "$TOKEN_OK" != true ]; then
    log_warn "Skipping app-repository registration (needs SUPERVISOR_TOKEN)."
    add_leftover "Register the two app repositories (Settings → Apps → App Store → ⋮ → Repositories): ${FRIGATE_REPO} and ${SECURACV_ADDON_REPO}"
    return 0
  fi
  local existing
  existing="$(curl -fsS -H "Authorization: Bearer ${SUPERVISOR_TOKEN:-}" \
    "${SUPERVISOR_URL}/store/repositories" 2>/dev/null || true)"
  local url
  for url in "$FRIGATE_REPO" "$SECURACV_ADDON_REPO"; do
    if printf '%s' "$existing" | grep -qF "$url"; then
      log_ok "App repository already registered: ${url} — skipping"
    elif curl -fsS -X POST \
        -H "Authorization: Bearer ${SUPERVISOR_TOKEN:-}" \
        -H "Content-Type: application/json" \
        -d "{\"repository\": \"${url}\"}" \
        "${SUPERVISOR_URL}/store/repositories" >/dev/null 2>&1; then
      log_ok "Registered app repository: ${url}"
    else
      log_warn "Could not register ${url} — add it under Settings → Apps → App Store → ⋮ → Repositories, then re-run."
      add_leftover "Register the app repository ${url} (Settings → Apps → App Store → ⋮ → Repositories), then re-run this installer."
    fi
  done
}

fb_install_addon() {
  # fb_install_addon <slug> <friendly name>
  # Returns 0 when the add-on is installed (now or already), 1 when it is not.
  local slug="$1" friendly="$2"
  local state
  state="$(addon_state "$slug")"

  if [ "$state" != "not_installed" ]; then
    log_ok "${friendly} already installed (state: ${state}) — skipping install"
    return 0
  fi
  log_info "Installing ${friendly} (${slug})…"
  if ! ha_cmd addons install "$slug"; then
    log_warn "Could not install ${friendly} — its repository may not be registered yet. Install it from Settings → Apps → App Store, or re-run this installer."
    add_leftover "Install ${friendly} from Settings → Apps → App Store (its repository may need registering first)."
    return 1
  fi
  log_ok "${friendly} installed"
  return 0
}

fb_start_addon() {
  # fb_start_addon <slug> <friendly name>
  local slug="$1" friendly="$2"
  if [ "$(addon_state "$slug")" = "started" ]; then
    log_ok "${friendly} already running — skipping start"
    return 0
  fi
  if ha_cmd addons start "$slug"; then
    log_ok "${friendly} started"
  else
    log_warn "Could not start ${friendly} — start it from Settings → Apps → ${friendly} → Start."
    add_leftover "Start ${friendly}: Settings → Apps → ${friendly} → Start."
  fi
}

fb_seed_frigate_config() {
  local src="${SRC_ROOT}/homeassistant/frigate/config.yaml"
  local dest_dir="${ADDON_CONFIGS_DIR}/${FRIGATE_SLUG}"
  if [ ! -f "$src" ]; then
    log_warn "Curated Frigate config not found in the source archive — skipping."
    return 0
  fi
  # Frigate accepts config.yml OR config.yaml; either one present means the
  # user already has a config we must never overwrite.
  if [ -f "${dest_dir}/config.yml" ] || [ -f "${dest_dir}/config.yaml" ]; then
    log_ok "Frigate already has a config in ${dest_dir} — never overwriting your edits"
    return 0
  fi
  mkdir -p "$dest_dir"
  if cp "$src" "${dest_dir}/config.yml"; then
    log_ok "Curated Frigate config placed at ${dest_dir}/config.yml (recordings and snapshots OFF by default — claims, not recordings)"
  else
    log_warn "Could not write ${dest_dir}/config.yml — paste the curated config into Frigate's Web UI config editor instead."
    add_leftover "Give Frigate its config: paste homeassistant/frigate/config.yaml into the Frigate Web UI config editor."
  fi
}

fb_set_kernel_mode() {
  if [ "$KERNEL_ARCH_OK" != true ]; then
    return 0
  fi
  if [ "$TOKEN_OK" != true ]; then
    log_warn "Cannot set the kernel's mode without SUPERVISOR_TOKEN — the app's setup wizard (Open Web UI) will walk you through it."
    add_leftover "Set the witness kernel to frigate mode via its setup wizard: Settings → Apps → Privacy Witness Kernel → Open Web UI."
    return 0
  fi
  if ! have jq; then
    log_warn "jq is not available, so the kernel's options cannot be merged safely here — the app's setup wizard (Open Web UI) will walk you through choosing frigate mode."
    add_leftover "Set the witness kernel to frigate mode via its setup wizard: Settings → Apps → Privacy Witness Kernel → Open Web UI."
    return 0
  fi
  local info current merged
  info="$(curl -fsS -H "Authorization: Bearer ${SUPERVISOR_TOKEN:-}" \
    "${SUPERVISOR_URL}/addons/${SECURACV_ADDON_SLUG}/info" 2>/dev/null || true)"
  if [ -z "$info" ]; then
    log_warn "Could not read the kernel app's options — set frigate mode via its setup wizard (Open Web UI)."
    add_leftover "Set the witness kernel to frigate mode via its setup wizard: Settings → Apps → Privacy Witness Kernel → Open Web UI."
    return 0
  fi
  current="$(printf '%s' "$info" | jq -r '.data.options.mode // ""')"
  if [ "$current" = "frigate" ]; then
    log_ok "Witness kernel already in frigate mode — skipping"
    return 0
  fi
  # Merge, never replace: change only `mode`, keep every option the user set.
  merged="$(printf '%s' "$info" | jq -c '{options: ((.data.options // {}) + {mode: "frigate"})}')"
  if curl -fsS -X POST \
      -H "Authorization: Bearer ${SUPERVISOR_TOKEN:-}" \
      -H "Content-Type: application/json" \
      -d "$merged" \
      "${SUPERVISOR_URL}/addons/${SECURACV_ADDON_SLUG}/options" >/dev/null 2>&1; then
    log_ok "Witness kernel set to frigate mode (it consumes Frigate detections instead of waiting in its wizard)"
  else
    log_warn "Could not set the kernel's mode — choose frigate mode in its setup wizard (Open Web UI)."
    add_leftover "Set the witness kernel to frigate mode via its setup wizard: Settings → Apps → Privacy Witness Kernel → Open Web UI."
  fi
}

ha_provision_fallback() {
  log_info "Fallback provisioner: the ha CLI plus direct Supervisor API calls. It covers repositories, the three apps, Frigate's config and the kernel's mode."

  fb_register_repositories

  # Order matters (the plan says why): broker before publishers; Frigate's
  # config before Frigate starts, or it crash-loops with nothing to do; the
  # kernel's mode before its start, or it boots into the wizard-only path.
  if fb_install_addon "$MOSQUITTO_SLUG" "Mosquitto broker"; then
    fb_start_addon "$MOSQUITTO_SLUG" "Mosquitto broker"
  fi

  if fb_install_addon "$FRIGATE_SLUG" "Frigate"; then
    fb_seed_frigate_config
    fb_start_addon "$FRIGATE_SLUG" "Frigate"
  else
    fb_seed_frigate_config
  fi

  if [ "$KERNEL_ARCH_OK" = true ]; then
    if fb_install_addon "$SECURACV_ADDON_SLUG" "Privacy Witness Kernel"; then
      fb_set_kernel_mode
      fb_start_addon "$SECURACV_ADDON_SLUG" "Privacy Witness Kernel"
    fi
  else
    log_warn "Skipping the witness kernel app on this CPU (armv7) — it ships amd64/aarch64 builds only."
    add_leftover "Run the witness kernel on an amd64/aarch64 machine (this CPU is armv7)."
  fi

  # The 'canary' broker login needs a JSON read-merge-write of Mosquitto's
  # logins list, which this fallback cannot do safely without Python. Honesty
  # over silence: say so, and say exactly where to do it.
  log_warn "The 'canary' broker login was NOT created on this fallback path (it needs a JSON merge the full installer performs)."
  add_leftover "Create the broker login your Canaries use: Settings → Apps → Mosquitto broker → Configuration → Logins → add username 'canary' with a password of your choice."
}

# --- files into /config -----------------------------------------------------

read_manifest_version() {
  local v
  v="$(grep -o '"version"[[:space:]]*:[[:space:]]*"[^"]*"' "$1" 2>/dev/null | grep -o '[0-9][0-9a-z.-]*' | head -n1 || true)"
  printf '%s' "${v:-unknown}"
}

ha_install_integration() {
  log_step "Install the SecuraCV integration — the custom component that turns witness events into Home Assistant entities"

  local src="${SRC_ROOT}/custom_components/securacv"
  local dest="${HA_CONFIG_DIR}/custom_components/securacv"
  if [ ! -d "$src" ] || [ ! -f "${src}/manifest.json" ]; then
    log_warn "The source archive has no custom_components/securacv — skipping (install it via HACS instead)."
    add_leftover "Install the SecuraCV integration via HACS (the downloaded archive did not contain it)."
    return 0
  fi

  local new_ver old_ver
  new_ver="$(read_manifest_version "${src}/manifest.json")"
  if [ -f "${dest}/manifest.json" ]; then
    old_ver="$(read_manifest_version "${dest}/manifest.json")"
  else
    old_ver=""
  fi

  mkdir -p "${HA_CONFIG_DIR}/custom_components"
  # Stage-then-swap: the live directory is never half-copied.
  local staging="${dest}.securacv-staging.$$"
  rm -rf "$staging"
  cp -R "$src" "$staging"
  # The integration's test suite and any bytecode caches ride along in the
  # source archive but have no business in a running Home Assistant's
  # config directory (HACS does not ship them either).
  rm -rf "${staging}/tests" "${staging}/__pycache__"
  rm -rf "$dest"
  mv "$staging" "$dest"

  if [ -z "$old_ver" ]; then
    log_ok "Integration installed (v${new_ver}) at ${dest}"
  elif [ "$old_ver" = "$new_ver" ]; then
    log_ok "Integration already at v${new_ver} — refreshed in place"
  else
    log_ok "Integration updated: v${old_ver} → v${new_ver}"
  fi
}

ha_install_blueprints() {
  log_step "Install automation blueprints — ready-made automations (daily digest, alerts) you wire up with two clicks, never overwriting your edits"

  local src_dir="${SRC_ROOT}/docs/blueprints"
  local dest_dir="${HA_CONFIG_DIR}/blueprints/automation/securacv"
  if [ ! -d "$src_dir" ]; then
    log_warn "No blueprints in the source archive — skipping."
    return 0
  fi
  mkdir -p "$dest_dir"
  local found=false f name
  for f in "$src_dir"/*.yaml; do
    [ -e "$f" ] || continue
    found=true
    name="$(basename "$f")"
    if [ -f "${dest_dir}/${name}" ]; then
      log_ok "Blueprint ${name} already present — keeping yours"
    else
      cp "$f" "${dest_dir}/${name}"
      log_ok "Installed blueprint ${name}"
    fi
  done
  [ "$found" = true ] || log_warn "No blueprint .yaml files found in the archive."
}

# The exact snippet a user adds when configuration.yaml already has its own
# lovelace: block (we must not append a second top-level key).
print_dashboard_snippet() {
  printf '    securacv-witness:\n'
  printf '      mode: yaml\n'
  printf '      title: SecuraCV\n'
  printf '      icon: mdi:shield-check\n'
  printf '      filename: securacv/dashboards/securacv-dashboard.yaml\n'
}

ha_register_dashboard() {
  local conf="${HA_CONFIG_DIR}/configuration.yaml"
  local marker='# BEGIN securacv-dashboards'

  if [ ! -f "$conf" ]; then
    log_warn "No configuration.yaml at ${conf} — cannot register the dashboard automatically."
    add_leftover "Register the dashboard by adding a lovelace: dashboards: entry for securacv/dashboards/securacv-dashboard.yaml to configuration.yaml."
    return 0
  fi

  if grep -qF "$marker" "$conf"; then
    log_ok "Dashboard already registered in configuration.yaml (by this installer) — skipping"
    return 0
  fi

  if grep -Eq '^lovelace:' "$conf"; then
    log_info "configuration.yaml already defines 'lovelace:' — not touching it. To show the SecuraCV dashboard, add this under your lovelace: dashboards: key:"
    print_dashboard_snippet
    add_leftover "Add the SecuraCV dashboard snippet (printed above) under the existing lovelace: dashboards: key in configuration.yaml."
    return 0
  fi

  # Append a clearly-marked block, validate, and roll back if Home Assistant
  # rejects it — configuration.yaml is the user's file, so we prove the edit
  # safe or undo it. The .bak exists only for the duration of the operation.
  local bak="${conf}.securacv.bak"
  cp "$conf" "$bak"
  {
    printf '\n'
    printf '# BEGIN securacv-dashboards — added by the SecuraCV installer; removing this block unregisters the dashboard.\n'
    printf 'lovelace:\n'
    printf '  dashboards:\n'
    printf '    securacv-witness:\n'
    printf '      mode: yaml\n'
    printf '      title: SecuraCV\n'
    printf '      icon: mdi:shield-check\n'
    printf '      show_in_sidebar: true\n'
    printf '      filename: securacv/dashboards/securacv-dashboard.yaml\n'
    printf '# END securacv-dashboards\n'
  } >> "$conf"

  log_info "Validating configuration.yaml with 'ha core check' (so a bad edit never takes your Home Assistant down)…"
  if ha_cmd core check >/dev/null 2>&1; then
    rm -f "$bak"
    log_ok "Dashboard registered in configuration.yaml — it appears in the sidebar as 'SecuraCV' after restart"
  else
    # Restore the pre-append snapshot: this removes exactly the block we
    # appended and nothing else.
    mv "$bak" "$conf"
    log_warn "'ha core check' rejected the change — configuration.yaml was restored unchanged."
    add_leftover "Register the dashboard by hand: add a lovelace: dashboards: entry for securacv/dashboards/securacv-dashboard.yaml to configuration.yaml (the automatic edit failed validation)."
  fi
}

ha_install_dashboards() {
  log_step "Install dashboards — the SecuraCV panels, kept in their own folder and registered without disturbing your configuration"

  local src_dir="${SRC_ROOT}/homeassistant/lovelace"
  local dest_dir="${HA_CONFIG_DIR}/securacv/dashboards"
  if [ ! -d "$src_dir" ]; then
    log_warn "No dashboards in the source archive — skipping."
    return 0
  fi
  mkdir -p "$dest_dir"
  local f name
  for f in "$src_dir"/*.yaml; do
    [ -e "$f" ] || continue
    name="$(basename "$f")"
    if [ -f "${dest_dir}/${name}" ]; then
      cp "$f" "${dest_dir}/${name}"
      log_ok "Dashboard ${name} refreshed"
    else
      cp "$f" "${dest_dir}/${name}"
      log_ok "Installed dashboard ${name}"
    fi
  done

  ha_register_dashboard
}

# --- restart + onboarding ---------------------------------------------------

ha_restart_core() {
  log_step "Restart Home Assistant — required once so it loads the SecuraCV integration"

  if ! ha_cmd core restart >/dev/null 2>&1; then
    log_warn "Could not restart Home Assistant from here — restart it via Settings → System → Restart, then the integration loads."
    add_leftover "Restart Home Assistant (Settings → System → Restart) so the SecuraCV integration loads."
    return 0
  fi
  log_ok "Restart requested"

  if [ "$TOKEN_OK" = true ] && [ -n "$ONBOARD_PY" ] && have python3; then
    if ! python3 "$ONBOARD_PY" wait-core --timeout 300; then
      log_warn "Home Assistant did not answer within 300 s — it may still be starting. The finishing step below may be incomplete; re-running this installer later completes it."
    fi
  else
    log_warn "Skipping the wait for core to come back (needs SUPERVISOR_TOKEN and python3) — give Home Assistant a minute before the next step's results appear."
  fi
}

ha_finish() {
  log_step "Finish onboarding — create the SecuraCV config entry and the daily-digest automation through the Home Assistant API"

  if [ "$TOKEN_OK" != true ]; then
    log_warn "Skipping (needs SUPERVISOR_TOKEN)."
    add_leftover "Add the SecuraCV integration: Settings → Devices & Services → Add integration → SecuraCV (choose Automatic)."
    return 0
  fi
  if [ -z "$ONBOARD_PY" ] || ! have python3; then
    log_warn "The onboarding helper is unavailable (needs python3 and scripts/ha_onboard.py) — finish in the UI instead."
    add_leftover "Add the SecuraCV integration: Settings → Devices & Services → Add integration → SecuraCV (choose Automatic)."
    return 0
  fi

  if python3 "$ONBOARD_PY" finish --config-dir "$HA_CONFIG_DIR"; then
    log_ok "Onboarding finished"
  else
    log_warn "Some onboarding could not complete — the lines above say exactly what and what to click. Re-running this installer is safe."
    add_leftover "Finish the onboarding steps the helper listed above (or re-run this installer once the cause is fixed)."
  fi
}

detect_ha_ip() {
  # A LAN address the user can actually browse to: any IPv4 that is not the
  # Supervisor's internal 172.30.x.x network (and not loopback).
  local ip
  ip="$(ha_cmd network info 2>/dev/null \
    | grep -oE '([0-9]{1,3}\.){3}[0-9]{1,3}' \
    | grep -vE '^172\.30\.' \
    | grep -vE '^127\.' \
    | head -n1 || true)"
  printf '%s' "${ip:-homeassistant.local}"
}

ha_summary() {
  local ip
  ip="$(detect_ha_ip)"

  printf '\n%b%b━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━%b\n' "$GREEN" "$BOLD" "$NC"
  printf '%b%b  SecuraCV install complete!%b\n' "$GREEN" "$BOLD" "$NC"
  printf '%b%b━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━%b\n\n' "$GREEN" "$BOLD" "$NC"

  printf '  %bWhere everything is:%b\n' "$BOLD" "$NC"
  printf '  • Home Assistant:      http://%s:8123\n' "$ip"
  printf '  • SecuraCV dashboard:  the "SecuraCV" item in the Home Assistant sidebar\n'
  printf '  • The integration:     Settings → Devices & Services → SecuraCV\n'
  printf '  • The witness kernel:  Settings → Apps → Privacy Witness Kernel (Open Web UI to back up your signing key)\n\n'

  printf '  %bYour Canaries sign in to the broker as%b canary%b.%b\n' "$BOLD" "$NC" "$BOLD" "$NC"
  printf '  Read that login'"'"'s password when you flash a device — it lives in\n'
  printf '  Settings → Apps → Mosquitto broker → Configuration → Logins.\n'
  printf '  (This installer never prints it, so no transcript ever holds it.)\n\n'

  printf '  %bOne step is always yours:%b put your camera'"'"'s RTSP URL into Frigate'"'"'s\n' "$BOLD" "$NC"
  printf '  config (%s/%s/config.yml) and set enabled: true on it —\n' "$ADDON_CONFIGS_DIR" "$FRIGATE_SLUG"
  printf '  camera addresses and credentials are yours, and we won'"'"'t scan your network.\n\n'

  if [ "${#LEFTOVERS[@]}" -gt 0 ]; then
    printf '  %bLeft for you to finish (some steps degraded on this run):%b\n' "$YELLOW" "$NC"
    local i=1 item
    for item in "${LEFTOVERS[@]}"; do
      printf '  %d. %s\n' "$i" "$item"
      i=$((i + 1))
    done
    printf '\n'
  else
    printf '  %bBeyond that: nothing left to do.%b Every step above completed.\n\n' "$GREEN" "$NC"
  fi
}

ha_path() {
  STEP_NO=0
  STEP_TOTAL=8
  log_info "Home Assistant OS detected (the 'ha' CLI answers) — running the full unattended install."

  ha_check_prereqs

  log_step "Download the SecuraCV source archive — one download serves the provisioner, the integration, blueprints, dashboards and the curated Frigate config"
  fetch_source

  ha_provision
  ha_install_integration
  ha_install_blueprints
  ha_install_dashboards
  ha_restart_core
  ha_finish
  ha_summary
}

# ===========================================================================
# PATH 2 — standalone Docker (no Home Assistant required)
# ===========================================================================

COMPOSE=()

docker_detect_compose() {
  if docker compose version >/dev/null 2>&1; then
    COMPOSE=(docker compose)
    log_ok "'docker compose' plugin available"
  elif have docker-compose; then
    COMPOSE=(docker-compose)
    log_ok "legacy 'docker-compose' available"
  else
    die "Docker is present but neither 'docker compose' nor 'docker-compose' works. Install the compose plugin (https://docs.docker.com/compose/install/) and re-run."
  fi
}

docker_path() {
  STEP_NO=0
  STEP_TOTAL=4
  log_info "No Home Assistant OS here, but Docker is available — installing the SecuraCV sidecar (it works fine without Home Assistant)."

  log_step "Check Docker prerequisites — confirm the daemon answers and compose is available"
  if ! docker info >/dev/null 2>&1; then
    die "Docker is installed but the daemon is not answering (is it running? do you need sudo?). Fix that and re-run."
  fi
  log_ok "Docker daemon answering"
  docker_detect_compose

  local home="${SECURACV_HOME:-./securacv}"
  mkdir -p "$home"
  log_ok "Install directory: ${home}"

  log_step "Download the SecuraCV source archive — it carries the compose files this path deploys"
  fetch_source

  log_step "Choose the deployment — reuse your MQTT broker if one is running, otherwise bundle one"
  local broker broker_net src_compose plan_line
  broker="$(docker ps --format '{{.Names}} {{.Image}}' 2>/dev/null \
    | grep -iE 'mosquitto|emqx|nanomq' | head -n1 | awk '{print $1}' || true)"
  broker_net=""
  if [ -n "$broker" ]; then
    # Reusing the broker only works if the sidecar can resolve its container
    # name — which needs a user-defined Docker network. On the default
    # bridge, container names do not resolve across compose projects, so a
    # sidecar pointed there would just wait on DNS and give up.
    broker_net="$(docker inspect -f '{{range $k, $v := .NetworkSettings.Networks}}{{$k}}{{"\n"}}{{end}}' "$broker" 2>/dev/null \
      | grep -vE '^(bridge|host|none)?$' | head -n1 || true)"
    if [ -z "$broker_net" ]; then
      log_warn "Found broker container '${broker}', but it lives on Docker's default bridge, where container names don't resolve across projects."
      log_warn "Bundling a broker instead. To reuse '${broker}', put it on a named network (compose does this by default) and re-run."
      broker=""
    fi
  fi
  if [ -n "$broker" ]; then
    src_compose="${SRC_ROOT}/docker/sidecar/quickstart.compose.yml"
    plan_line="Found broker container '${broker}' on network '${broker_net}' — the sidecar joins that network and publishes to it (no second broker installed)."
  else
    src_compose="${SRC_ROOT}/docker/sidecar/quickstart-with-broker.compose.yml"
    plan_line="No reusable MQTT broker detected — bundling Mosquitto, bound to 127.0.0.1 so it stays off your LAN by default."
  fi
  log_info "$plan_line"
  [ -f "$src_compose" ] || die "The source archive is missing $(basename "$src_compose") — was the download truncated? Re-run to retry."

  # One confirmation when a human is watching; fully unattended otherwise.
  if [ -t 0 ] && [ -t 1 ]; then
    local answer
    read -r -p "Proceed with this plan in ${home}? [Y/n] " answer
    case "$answer" in
      n|N|no|No|NO) log_info "Stopped at your request — nothing was changed."; exit 0 ;;
    esac
  fi

  local dest_compose="${home}/compose.yml"
  if [ -f "$dest_compose" ]; then
    log_ok "compose.yml already exists in ${home} — keeping yours (delete it to regenerate)"
  else
    cp "$src_compose" "$dest_compose"
    if [ -n "$broker" ] && [ "$broker" != "mosquitto" ]; then
      # The .bak + rm form is portable across GNU and BSD/macOS sed.
      sed -i.securacv.bak "s/FRIGATE_MQTT_HOST: \"mosquitto\"/FRIGATE_MQTT_HOST: \"${broker}\"/" "$dest_compose"
      rm -f "${dest_compose}.securacv.bak"
    fi
    if [ -n "$broker" ]; then
      # Join the broker's network so its container name actually resolves
      # from the sidecar (this project's default network becomes that one).
      {
        printf '\n'
        printf '# Added by the SecuraCV installer: join the network your broker already\n'
        printf '# lives on, so the sidecar reaches it by container name.\n'
        printf 'networks:\n'
        printf '  default:\n'
        printf '    name: %s\n' "$broker_net"
        printf '    external: true\n'
      } >> "$dest_compose"
      log_ok "Sidecar joins existing Docker network '${broker_net}'"
    fi
    log_ok "Placed $(basename "$src_compose") as ${dest_compose}"
  fi

  log_step "Start the sidecar and check its health — 'doctor' verifies the broker connection and event flow"
  log_info "Starting containers ('${COMPOSE[*]} up -d')…"
  if ! "${COMPOSE[@]}" -f "$dest_compose" up -d; then
    die "Container start failed — the compose output above says why. Fix it and re-run (safe to repeat)."
  fi
  log_ok "Containers running"

  log_info "Running the sidecar's doctor (its output follows):"
  if ! "${COMPOSE[@]}" -f "$dest_compose" run --rm securacv doctor; then
    log_warn "doctor reported problems — its output above says exactly what to fix. Re-run it any time: ${COMPOSE[*]} -f ${dest_compose} run --rm securacv doctor"
  fi

  printf '\n%b%b━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━%b\n' "$GREEN" "$BOLD" "$NC"
  printf '%b%b  SecuraCV sidecar install complete!%b\n' "$GREEN" "$BOLD" "$NC"
  printf '%b%b━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━%b\n\n' "$GREEN" "$BOLD" "$NC"
  printf '  %bWhere your events go:%b sanitized witness events are published to the MQTT\n' "$BOLD" "$NC"
  printf '  broker and announced via MQTT discovery. Any Home Assistant that shares this\n'
  printf '  broker picks them up automatically — now or whenever you add one. No\n'
  printf '  reconfiguration needed.\n\n'
  printf '  %bSee events without Home Assistant:%b\n' "$BOLD" "$NC"
  if [ -n "$broker" ]; then
    printf '    mosquitto_sub -h <your-broker-host> -t '"'"'witness/#'"'"' -v\n'
    printf '    (the same host your existing broker container '"'"'%s'"'"' serves on)\n\n' "$broker"
  else
    printf '    mosquitto_sub -h localhost -t '"'"'witness/#'"'"' -v\n\n'
  fi
  printf '  A device signing key was generated inside the securacv_data volume on\n'
  printf '  first start — back it up (it is what makes your log verifiable).\n'
}

# ===========================================================================
# Environment detection + main
# ===========================================================================

explain_two_paths() {
  log_error "Neither Home Assistant OS nor Docker was found here."
  printf '\nSecuraCV installs in one of two places:\n\n'
  printf '  1. %bHome Assistant OS%b — open the Terminal (or SSH) add-on and run this\n' "$BOLD" "$NC"
  printf '     same one-liner there. The Supervisor lets the install run fully\n'
  printf '     unattended: broker, Frigate, witness kernel, integration, dashboards.\n\n'
  printf '  2. %bAny Docker host%b — install Docker (with the compose plugin) and re-run.\n' "$BOLD" "$NC"
  printf '     SecuraCV runs as a small sidecar next to your existing Frigate and\n'
  printf '     broker; no Home Assistant required.\n\n'
  printf 'Nothing on this machine was changed.\n'
}

main() {
  printf '\n%bSecuraCV installer%b (branch: %s)\n' "$BOLD" "$NC" "$SECURACV_BRANCH"
  printf 'Narrated and idempotent: every step says what and why, and re-running skips what is already done.\n'

  if have ha && ha_cmd core info >/dev/null 2>&1; then
    ha_path
  elif have docker; then
    docker_path
  else
    explain_two_paths
    exit 2
  fi
}

main "$@"
