#!/usr/bin/env bash
set -euo pipefail
# SecuraCV Easy Install
# Installs Frigate + Mosquitto + SecuraCV on Home Assistant OS in one command.
#
# Usage (from HA Terminal/SSH app):
#   curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash
#
# Or with explicit branch:
#   SECURACV_BRANCH=main bash <(curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh)
#
# Idempotent: safe to run multiple times.


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SECURACV_BRANCH="${SECURACV_BRANCH:-main}"
GITHUB_ORG="kmay89"
GITHUB_REPO="securaCV"
GITHUB_RAW="https://raw.githubusercontent.com/${GITHUB_ORG}/${GITHUB_REPO}/${SECURACV_BRANCH}"
GITHUB_API="https://api.github.com/repos/${GITHUB_ORG}/${GITHUB_REPO}/tarball/${SECURACV_BRANCH}"

HA_CONFIG_DIR="/config"
SECURACV_STATE_DIR="${HA_CONFIG_DIR}/.securacv"
COMPONENTS_DIR="${HA_CONFIG_DIR}/custom_components"
AUTOMATIONS_DIR="${HA_CONFIG_DIR}/automations"
LOVELACE_DIR="${HA_CONFIG_DIR}/lovelace"

MOSQUITTO_SLUG="core_mosquitto"
FRIGATE_REPO="https://github.com/blakeblackshear/frigate-hass-addons"
# Supervisor app slugs are "<repo-hash>_<addon-slug>" where repo-hash is
# sha1(lowercased repo URL)[:8] — ccab4aaf for blakeblackshear's repo,
# d0491a67 for this one.
FRIGATE_SLUG="ccab4aaf_frigate"
# The Frigate app (0.16+) reads its config from its own app config
# directory, NOT /config/frigate.yml. Visible from the Terminal/SSH app
# at /addon_configs once the Frigate app is installed.
FRIGATE_ADDON_CONFIG_DIR="/addon_configs/${FRIGATE_SLUG}"
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
# Logging helpers
# ---------------------------------------------------------------------------
log_info()  { printf "${BLUE}[SecuraCV]${NC} %s\n" "$*"; }
log_ok()    { printf "${GREEN}  ✓${NC} %s\n" "$*"; }
log_warn()  { printf "${YELLOW}  !${NC} %s\n" "$*"; }
log_error() { printf "${RED}  ✗${NC} %s\n" "$*" >&2; }
log_step()  { printf "\n${BOLD}▶ %s${NC}\n" "$*"; }

die() { log_error "$*"; exit 1; }

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
check_prerequisites() {
  log_step "Checking prerequisites"

  # Must have the ha CLI (present on HA OS)
  if ! command -v ha >/dev/null 2>&1; then
    die "The 'ha' CLI is not available. Run this script from the Home Assistant Terminal or SSH app."
  fi
  log_ok "ha CLI found"

  # Confirm Supervisor API is reachable
  if ! ha core info >/dev/null 2>&1; then
    die "Cannot reach the HA Supervisor API. Is Home Assistant OS running?"
  fi
  log_ok "HA Supervisor API reachable"

  # Architecture check
  ARCH=$(uname -m)
  case "$ARCH" in
    x86_64)   HA_ARCH="amd64"  ;;
    aarch64)  HA_ARCH="aarch64" ;;
    armv7l)   HA_ARCH="armv7"   ;;
    *)        die "Unsupported architecture: $ARCH. SecuraCV supports amd64 and aarch64 (Raspberry Pi 4/5)." ;;
  esac
  log_ok "Architecture: ${ARCH} (${HA_ARCH})"

  # curl required for downloads
  if ! command -v curl >/dev/null 2>&1; then
    die "'curl' is required but not found."
  fi
  log_ok "curl available"

  # openssl for key generation
  if ! command -v openssl >/dev/null 2>&1; then
    log_warn "openssl not found — device key will be generated using /dev/urandom"
    OPENSSL_AVAILABLE=false
  else
    OPENSSL_AVAILABLE=true
    log_ok "openssl available"
  fi
}

# ---------------------------------------------------------------------------
# Wait for an app to reach a specific state
# ---------------------------------------------------------------------------
addon_wait_state() {
  local slug="$1"
  local wanted_state="$2"
  local max_wait="${3:-60}"
  local waited=0

  while [ "$waited" -lt "$max_wait" ]; do
    local state
    state=$(ha addons info "$slug" 2>/dev/null | grep -E '^state:' | awk '{print $2}' || true)
    if [ "$state" = "$wanted_state" ]; then
      return 0
    fi
    sleep 3
    waited=$((waited + 3))
  done
  return 1
}

# ---------------------------------------------------------------------------
# Step 1: Mosquitto MQTT broker
# ---------------------------------------------------------------------------
install_mosquitto() {
  log_step "Installing Mosquitto MQTT broker"

  local state
  state=$(ha addons info "$MOSQUITTO_SLUG" 2>/dev/null | grep -E '^state:' | awk '{print $2}' || echo "not_installed")

  if [ "$state" = "started" ]; then
    log_ok "Mosquitto already running — skipping"
    return 0
  fi

  if [ "$state" = "not_installed" ]; then
    log_info "Installing Mosquitto app…"
    ha addons install "$MOSQUITTO_SLUG" || die "Failed to install Mosquitto"
    log_ok "Mosquitto installed"
  fi

  log_info "Starting Mosquitto…"
  ha addons start "$MOSQUITTO_SLUG" || die "Failed to start Mosquitto"

  if addon_wait_state "$MOSQUITTO_SLUG" "started" 60; then
    log_ok "Mosquitto running"
  else
    die "Mosquitto did not start within 60 s. Check: ha addons logs core_mosquitto"
  fi
}

# ---------------------------------------------------------------------------
# Step 2: Frigate NVR
# ---------------------------------------------------------------------------
install_frigate() {
  log_step "Installing Frigate NVR app"

  local state
  state=$(ha addons info "$FRIGATE_SLUG" 2>/dev/null | grep -E '^state:' | awk '{print $2}' || echo "not_installed")

  if [ "$state" != "not_installed" ] && [ -n "$state" ]; then
    log_ok "Frigate already installed (state: ${state}) — skipping"
    return 0
  fi

  # The ha CLI has no command to add an app repository, so if the Frigate
  # repo isn't registered yet the install below fails and we fall back to
  # manual instructions.
  log_info "Installing Frigate (this may take a few minutes)…"
  ha addons install "$FRIGATE_SLUG" || {
    log_warn "Automated Frigate install failed — its app repository is probably not added yet."
    log_warn "Add it: Settings → Apps → App Store → ⋮ → Repositories → ${FRIGATE_REPO}"
    log_warn "then install Frigate from the store (or re-run this script)."
    log_warn "SecuraCV setup will continue — complete Frigate setup before first run."
    return 0
  }
  log_ok "Frigate installed (configure cameras before starting)"
}

# ---------------------------------------------------------------------------
# Step 3: SecuraCV HACS integration — copy component files directly
# ---------------------------------------------------------------------------
install_integration() {
  log_step "Installing SecuraCV Home Assistant integration"

  local dest="${COMPONENTS_DIR}/securacv"

  if [ -d "$dest" ] && [ -f "${dest}/manifest.json" ]; then
    local installed_ver
    installed_ver=$(grep -o '"version": "[^"]*"' "${dest}/manifest.json" | grep -o '[0-9.]*' || echo "unknown")
    log_ok "SecuraCV integration already installed (v${installed_ver}) — updating to latest"
  fi

  mkdir -p "$COMPONENTS_DIR"

  log_info "Downloading SecuraCV integration files…"
  local tmpdir
  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT

  # Download the repo tarball and extract the custom_components/securacv directory
  if curl -fsSL "$GITHUB_API" | tar -xz -C "$tmpdir" --strip-components=2 \
      --wildcards "*/custom_components/securacv" 2>/dev/null; then
    if [ -d "${tmpdir}/securacv" ]; then
      rm -rf "$dest"
      cp -r "${tmpdir}/securacv" "$dest"
      log_ok "Integration installed to ${dest}"
    else
      die "Download succeeded but custom_components/securacv was not found in the archive."
    fi
  else
    die "Failed to download SecuraCV from GitHub. Check network connectivity."
  fi

  trap - EXIT
  rm -rf "$tmpdir"
}

# ---------------------------------------------------------------------------
# Step 4: SecuraCV Privacy Witness Kernel app
# ---------------------------------------------------------------------------
install_addon() {
  log_step "Installing SecuraCV Privacy Witness Kernel app"

  local state
  state=$(ha addons info "$SECURACV_ADDON_SLUG" 2>/dev/null | grep -E '^state:' | awk '{print $2}' || echo "not_installed")

  if [ "$state" != "not_installed" ] && [ -n "$state" ]; then
    log_ok "SecuraCV app already installed (state: ${state}) — skipping"
    return 0
  fi

  # As with Frigate, the ha CLI cannot add app repositories; if the
  # SecuraCV repo isn't registered yet the install fails with manual steps.
  log_info "Installing Privacy Witness Kernel app…"
  ha addons install "$SECURACV_ADDON_SLUG" || {
    log_warn "Automated app install failed — the SecuraCV app repository is probably not added yet."
    log_warn "Add it: Settings → Apps → App Store → ⋮ → Repositories → ${SECURACV_ADDON_REPO}"
    log_warn "then install 'Privacy Witness Kernel' from the store (or re-run this script)."
    return 0
  }
  log_ok "Privacy Witness Kernel app installed"

  log_info "Starting Privacy Witness Kernel app…"
  ha addons start "$SECURACV_ADDON_SLUG" || {
    log_warn "Could not start app automatically."
    log_warn "Start manually: Settings → Apps → Privacy Witness Kernel → Start"
    return 0
  }

  if addon_wait_state "$SECURACV_ADDON_SLUG" "started" 60; then
    log_ok "Privacy Witness Kernel running (setup wizard is now available)"
  else
    log_warn "App did not start within 60 s — check logs if needed"
  fi
}

# ---------------------------------------------------------------------------
# Step 5: Generate device key seed
# ---------------------------------------------------------------------------
generate_device_key() {
  log_step "Generating device signing key"

  mkdir -p "$SECURACV_STATE_DIR"
  chmod 700 "$SECURACV_STATE_DIR"

  local key_file="${SECURACV_STATE_DIR}/device_key"

  if [ -f "$key_file" ]; then
    log_ok "Device key already exists at ${key_file} — not overwriting"
    log_warn "⚠ IMPORTANT: Back up ${key_file} — losing it means losing the ability to verify your log."
    return 0
  fi

  local key
  if [ "$OPENSSL_AVAILABLE" = true ]; then
    key=$(openssl rand -hex 32)
  else
    key=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
  fi

  printf '%s\n' "$key" > "$key_file"
  chmod 600 "$key_file"

  log_ok "Device key written to ${key_file}"
  printf "\n${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
  printf "${YELLOW}⚠  BACK UP YOUR DEVICE KEY${NC}\n"
  printf "   File: ${key_file}\n"
  printf "   Key:  %s\n" "$key"
  printf "   Without this key you cannot verify your witness log.\n"
  printf "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n\n"
}

# ---------------------------------------------------------------------------
# Step 6: Drop config templates
# ---------------------------------------------------------------------------
deploy_configs() {
  log_step "Deploying configuration templates"

  local key_file="${SECURACV_STATE_DIR}/device_key"
  local device_key=""
  if [ -f "$key_file" ]; then
    device_key=$(cat "$key_file")
  fi

  # Frigate config template (only if Frigate doesn't already have a config)
  local frigate_conf="${HA_CONFIG_DIR}/frigate.yml"
  if [ ! -f "$frigate_conf" ]; then
    log_info "Writing Frigate config template to ${frigate_conf}"
    cat > "$frigate_conf" << 'EOF'
# Frigate NVR configuration — generated by SecuraCV install (fallback template).
#
# The canonical, curated config is homeassistant/frigate/config.yaml in this
# repo, and the plan-driven installer (canary-local/tools/hub_seed_apply.py)
# copies THAT verbatim. This inline copy exists only for the bash one-liner
# path — keep its privacy defaults in step with the curated file.
#
# Full documentation: https://docs.frigate.video/configuration/

mqtt:
  enabled: true
  host: core-mosquitto      # Mosquitto app hostname
  port: 1883
  # username: your-mqtt-user    # Uncomment if you set Mosquitto credentials
  # password: your-mqtt-pass

# securaCV is "claims, not recordings": detection and MQTT events work fully
# with NO raw imagery retained, so recordings AND snapshots ship OFF. Turn
# either on only if you deliberately accept storing images — and use an
# NVMe/SSD if you do, since continuous recording destroys SD cards.
record:
  enabled: false

snapshots:
  enabled: false

cameras:
  # Replace with your camera. `detect` role only — no `record` — matching the
  # privacy default above. Add more cameras by copying this block.
  front_door:
    ffmpeg:
      inputs:
        - path: rtsp://PLACEHOLDER_CAMERA_IP/stream
          roles:
            - detect
    detect:
      width: 1280
      height: 720
      fps: 5

# Object detection: built-in CPU detector. For real-time detection use a Coral
# USB TPU (`type: edgetpu`, `device: usb`) or a Jetson running Frigate-TensorRT
# that publishes to this same broker — see integrations/jetson-detector/.
detectors:
  cpu1:
    type: cpu
    num_threads: 2
EOF
    log_ok "Frigate config template written"
  else
    log_ok "Frigate config template already exists — not overwriting"
  fi

  # The Frigate app does NOT read /config/frigate.yml — it reads
  # config.yml inside its own app config directory. If that directory is
  # visible (Frigate app installed and /addon_configs mapped into this
  # shell), seed it with the same template so Frigate picks it up directly.
  if [ -d "$FRIGATE_ADDON_CONFIG_DIR" ]; then
    if [ ! -f "${FRIGATE_ADDON_CONFIG_DIR}/config.yml" ] && [ ! -f "${FRIGATE_ADDON_CONFIG_DIR}/config.yaml" ]; then
      if cp "$frigate_conf" "${FRIGATE_ADDON_CONFIG_DIR}/config.yml" 2>/dev/null; then
        log_ok "Frigate config seeded at ${FRIGATE_ADDON_CONFIG_DIR}/config.yml"
      else
        log_warn "Could not write ${FRIGATE_ADDON_CONFIG_DIR}/config.yml (permissions?)."
        log_warn "Copy ${frigate_conf} there manually, or paste it into the Frigate Web UI config editor."
      fi
    else
      log_ok "Frigate already has a config in ${FRIGATE_ADDON_CONFIG_DIR} — not overwriting"
    fi
  else
    log_warn "Frigate reads its config from ${FRIGATE_ADDON_CONFIG_DIR}/config.yml — copy"
    log_warn "the template there (or paste it into the Frigate Web UI config editor)"
    log_warn "after installing Frigate."
  fi

  # Install automations
  install_automations

  # Install Lovelace dashboard
  install_dashboard

  log_ok "Configuration templates deployed"
}

# ---------------------------------------------------------------------------
# Step 7: Install notification automations
# ---------------------------------------------------------------------------
install_automations() {
  mkdir -p "$AUTOMATIONS_DIR"

  local digest_file="${AUTOMATIONS_DIR}/securacv_daily_digest.yaml"
  local pattern_file="${AUTOMATIONS_DIR}/securacv_pattern_break.yaml"
  local integrity_file="${AUTOMATIONS_DIR}/securacv_integrity_failure.yaml"

  if [ ! -f "$digest_file" ]; then
    log_info "Installing daily digest automation…"
    curl -fsSL "${GITHUB_RAW}/homeassistant/automations/securacv_daily_digest.yaml" \
      -o "$digest_file" 2>/dev/null || {
      log_warn "Could not download daily digest automation (network issue?)"
    }
  fi

  if [ ! -f "$pattern_file" ]; then
    log_info "Installing pattern-break alert automation…"
    curl -fsSL "${GITHUB_RAW}/homeassistant/automations/securacv_pattern_break.yaml" \
      -o "$pattern_file" 2>/dev/null || {
      log_warn "Could not download pattern-break automation (network issue?)"
    }
  fi

  if [ ! -f "$integrity_file" ]; then
    log_info "Installing integrity-failure alert automation…"
    curl -fsSL "${GITHUB_RAW}/homeassistant/automations/securacv_integrity_failure.yaml" \
      -o "$integrity_file" 2>/dev/null || {
      log_warn "Could not download integrity-failure automation (network issue?)"
    }
  fi
}

# ---------------------------------------------------------------------------
# Step 8: Install Lovelace dashboard
# ---------------------------------------------------------------------------
install_dashboard() {
  mkdir -p "$LOVELACE_DIR"

  local dash_file="${LOVELACE_DIR}/securacv-dashboard.yaml"
  if [ ! -f "$dash_file" ]; then
    log_info "Installing SecuraCV Lovelace dashboard…"
    curl -fsSL "${GITHUB_RAW}/homeassistant/lovelace/securacv-dashboard.yaml" \
      -o "$dash_file" 2>/dev/null || {
      log_warn "Could not download Lovelace dashboard (network issue?)"
    }
  else
    log_ok "Lovelace dashboard already installed — not overwriting"
  fi
}

# ---------------------------------------------------------------------------
# Summary / next steps
# ---------------------------------------------------------------------------
print_next_steps() {
  # Try to detect HA IP from supervisor
  local ha_ip
  ha_ip=$(ha network info 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1 || echo "homeassistant.local")

  printf "\n${GREEN}${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
  printf "${GREEN}${BOLD}  SecuraCV install complete!${NC}\n"
  printf "${GREEN}${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n\n"

  printf "${BOLD}Next steps:${NC}\n\n"

  printf "  1. ${BOLD}Configure Frigate cameras${NC}\n"
  printf "     Frigate reads:  ${FRIGATE_ADDON_CONFIG_DIR}/config.yml\n"
  printf "     (template also at ${HA_CONFIG_DIR}/frigate.yml — same contents)\n"
  printf "     Replace the placeholder RTSP URLs with your cameras'.\n"
  printf "     Then start Frigate: Settings → Apps → Frigate → Start\n\n"

  printf "  2. ${BOLD}Add the SecuraCV integration${NC}\n"
  printf "     Open: http://${ha_ip}:8123\n"
  printf "     Go to: Settings → Devices & Services → Add Integration → SecuraCV\n\n"

  printf "  3. ${BOLD}Run the setup wizard${NC}\n"
  printf "     Start the Privacy Witness Kernel app:\n"
  printf "     Settings → Apps → Privacy Witness Kernel → Start\n"
  printf "     Then open the wizard: Settings → Apps → Privacy Witness Kernel → Open Web UI\n\n"

  printf "  4. ${BOLD}Restart Home Assistant${NC}\n"
  printf "     Settings → System → Restart\n"
  printf "     The SecuraCV sensors will appear after restart.\n\n"

  printf "  5. ${BOLD}Add the SecuraCV dashboard${NC}\n"
  printf "     Settings → Dashboards → Add Dashboard → From file\n"
  printf "     File: ${LOVELACE_DIR}/securacv-dashboard.yaml\n\n"

  printf "${YELLOW}⚠  Remember to back up your device key:${NC}\n"
  printf "   ${SECURACV_STATE_DIR}/device_key\n\n"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
  printf "\n${BOLD}SecuraCV Easy Install${NC} (branch: ${SECURACV_BRANCH})\n"
  printf "This script will install Mosquitto, Frigate, and SecuraCV on your Home Assistant OS.\n\n"

  check_prerequisites
  install_mosquitto
  install_frigate
  install_integration
  install_addon
  generate_device_key
  deploy_configs
  print_next_steps
}

main "$@"
