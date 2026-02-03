#!/bin/bash
#
# SecuraCV Canary — OTA Auto-Deploy Script (Linux/macOS)
#
# Watches for new firmware builds and automatically deploys via OTA.
# Requires: inotifywait (inotify-tools) on Linux, fswatch on macOS
#
# Usage:
#   ./ota_deploy.sh [device_ip]
#   ./ota_deploy.sh 192.168.4.1
#   CANARY_IP=192.168.4.1 ./ota_deploy.sh
#
# Copyright (c) 2026 ERRERlabs / Karl May
# License: Apache-2.0

set -euo pipefail

# Configuration
CANARY_IP="${CANARY_IP:-${1:-192.168.4.1}}"
CANARY_PORT="${CANARY_PORT:-80}"
OTA_ENDPOINT="/api/ota"
BUILD_DIR="${BUILD_DIR:-.pio/build}"
FIRMWARE_NAME="firmware.bin"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
  echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
  echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
  echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
  echo -e "${RED}[ERROR]${NC} $1"
}

# Find the most recent firmware binary
find_firmware() {
  local latest=""
  local latest_time=0

  for env_dir in "$BUILD_DIR"/*/; do
    local fw_path="${env_dir}${FIRMWARE_NAME}"
    if [[ -f "$fw_path" ]]; then
      local mtime
      mtime=$(stat -c %Y "$fw_path" 2>/dev/null || stat -f %m "$fw_path" 2>/dev/null)
      if [[ "$mtime" -gt "$latest_time" ]]; then
        latest_time="$mtime"
        latest="$fw_path"
      fi
    fi
  done

  echo "$latest"
}

# Deploy firmware via OTA
deploy_firmware() {
  local firmware_path="$1"
  local firmware_size
  firmware_size=$(stat -c %s "$firmware_path" 2>/dev/null || stat -f %z "$firmware_path" 2>/dev/null)

  log_info "Deploying: $firmware_path ($firmware_size bytes)"
  log_info "Target: http://${CANARY_IP}:${CANARY_PORT}${OTA_ENDPOINT}"

  # Upload firmware using curl
  local response
  local http_code

  response=$(curl -s -w "\n%{http_code}" \
    --connect-timeout 10 \
    --max-time 120 \
    -X POST \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@${firmware_path}" \
    "http://${CANARY_IP}:${CANARY_PORT}${OTA_ENDPOINT}" 2>&1)

  http_code=$(echo "$response" | tail -n1)
  response=$(echo "$response" | head -n-1)

  if [[ "$http_code" == "200" ]]; then
    log_success "OTA update successful!"
    log_info "Response: $response"
    log_info "Device will reboot..."
    return 0
  else
    log_error "OTA update failed (HTTP $http_code)"
    log_error "Response: $response"
    return 1
  fi
}

# Check device connectivity
check_device() {
  log_info "Checking device at ${CANARY_IP}..."

  if ping -c 1 -W 2 "$CANARY_IP" &>/dev/null; then
    log_success "Device is reachable"
    return 0
  else
    log_warn "Device not responding to ping (may be normal)"
    return 0
  fi
}

# Watch for file changes and deploy
watch_and_deploy() {
  log_info "Watching for firmware changes in: $BUILD_DIR"
  log_info "Press Ctrl+C to stop"
  echo

  local last_deployed=""

  # Detect platform and use appropriate watcher
  if command -v inotifywait &>/dev/null; then
    # Linux with inotify-tools
    while true; do
      inotifywait -q -e close_write,moved_to -r "$BUILD_DIR" --include ".*\.bin$" || true

      sleep 1  # Wait for build to complete

      local firmware
      firmware=$(find_firmware)
      if [[ -n "$firmware" && "$firmware" != "$last_deployed" ]]; then
        echo
        log_info "New firmware detected!"
        if deploy_firmware "$firmware"; then
          last_deployed="$firmware"
        fi
        echo
      fi
    done

  elif command -v fswatch &>/dev/null; then
    # macOS with fswatch
    fswatch -0 -e ".*" -i "\\.bin$" "$BUILD_DIR" | while read -d "" path; do
      sleep 1  # Wait for build to complete

      local firmware
      firmware=$(find_firmware)
      if [[ -n "$firmware" && "$firmware" != "$last_deployed" ]]; then
        echo
        log_info "New firmware detected!"
        if deploy_firmware "$firmware"; then
          last_deployed="$firmware"
        fi
        echo
      fi
    done

  else
    log_error "No file watcher found!"
    log_error "Install inotify-tools (Linux) or fswatch (macOS):"
    log_error "  Ubuntu/Debian: sudo apt install inotify-tools"
    log_error "  macOS: brew install fswatch"
    exit 1
  fi
}

# Single deployment mode
single_deploy() {
  local firmware
  firmware=$(find_firmware)

  if [[ -z "$firmware" ]]; then
    log_error "No firmware found in $BUILD_DIR"
    log_info "Run 'pio run' to build first"
    exit 1
  fi

  check_device
  deploy_firmware "$firmware"
}

# Main
main() {
  echo
  echo "╔══════════════════════════════════════════════════════════════╗"
  echo "║       SecuraCV Canary — OTA Auto-Deploy                      ║"
  echo "╚══════════════════════════════════════════════════════════════╝"
  echo

  # Check for required tools
  if ! command -v curl &>/dev/null; then
    log_error "curl is required but not installed"
    exit 1
  fi

  # Parse arguments
  case "${1:-}" in
    --watch|-w)
      shift
      CANARY_IP="${1:-$CANARY_IP}"
      watch_and_deploy
      ;;
    --help|-h)
      echo "Usage: $0 [options] [device_ip]"
      echo
      echo "Options:"
      echo "  --watch, -w    Watch for changes and auto-deploy"
      echo "  --help, -h     Show this help"
      echo
      echo "Environment variables:"
      echo "  CANARY_IP      Device IP (default: 192.168.4.1)"
      echo "  CANARY_PORT    HTTP port (default: 80)"
      echo "  BUILD_DIR      PlatformIO build directory (default: .pio/build)"
      echo
      exit 0
      ;;
    *)
      single_deploy
      ;;
  esac
}

main "$@"
