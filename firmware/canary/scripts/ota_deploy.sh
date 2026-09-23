#!/bin/bash
set -euo pipefail
#
# SecuraCV Canary — OTA Auto-Deploy Script (Linux/macOS)
#
# Pushes a development build to a Canary's dev-only `POST /api/ota` endpoint
# (release images have no such route), once or on every new build.
# Requires: curl; openssl for a TLS device; inotifywait (inotify-tools) on
# Linux or fswatch on macOS for --watch.
#
# The endpoint is behind the device's API bearer token:
#   CANARY_TOKEN   the `token` from the Canary's recovery kit
#                  (canary-recovery-kit.json). From the environment, or asked
#                  for at the terminal without echo. Never an argument, and
#                  handed to curl on stdin (-H @-), never on its command line
#                  (argv is visible to every process on the machine).
# A dev build serves its API over self-signed HTTPS on 443 once setup is done
# (port 80 then only redirects). Such a device is spoken to over HTTPS only,
# and only when its certificate matches a pin:
#   CANARY_TLS_FP  the device's `tls_cert_fp` (SHA-256 of its certificate, 64
#                  hex digits; colons allowed), from the recovery kit or
#                  /api/status. With it set, the script never falls back to
#                  plain HTTP.
# Without a pin, a device that answers TLS on 443 is refused (its certificate
# fingerprint is printed so it can be compared with the recovery kit); a
# device with nothing on 443 is an HTTP-only build and gets plain HTTP.
#
# Usage:
#   CANARY_TOKEN=... ./ota_deploy.sh [device_ip]
#   CANARY_TOKEN=... CANARY_TLS_FP=... ./ota_deploy.sh --watch 192.168.4.1
#   CANARY_IP=192.168.4.1 ./ota_deploy.sh          # asks for the token
#
# Copyright (c) 2026 ERRERlabs / Karl May
# License: Apache-2.0


# Configuration
CANARY_IP="${CANARY_IP:-${1:-192.168.4.1}}"
CANARY_PORT="${CANARY_PORT:-80}"
CANARY_HTTPS_PORT="${CANARY_HTTPS_PORT:-443}"
OTA_ENDPOINT="/api/ota"
STATUS_ENDPOINT="/api/status"
BUILD_DIR="${BUILD_DIR:-.pio/build}"
FIRMWARE_NAME="firmware.bin"

# Set by resolve_transport: where the device is spoken to, and (TLS only) the
# pin and the curl options that enforce it.
SCHEME="http"
PORT="$CANARY_PORT"
PIN=""
CURL_TLS=()

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

# The bearer token: $CANARY_TOKEN, else a no-echo prompt on a terminal.
read_token() {
  if [[ -z "${CANARY_TOKEN:-}" ]] && [[ -t 0 ]]; then
    read -r -s -p "Canary API token (the recovery kit's \`token\`; not echoed): " CANARY_TOKEN || true
    echo >&2
  fi
  # A pasted token often carries a trailing newline; the device's never holds
  # whitespace, and anything else non-printable would break the header.
  CANARY_TOKEN="${CANARY_TOKEN:-}"
  CANARY_TOKEN="${CANARY_TOKEN#"${CANARY_TOKEN%%[![:space:]]*}"}"
  CANARY_TOKEN="${CANARY_TOKEN%"${CANARY_TOKEN##*[![:space:]]}"}"
  if [[ -z "$CANARY_TOKEN" ]]; then
    log_error "No API token. Set CANARY_TOKEN to the \`token\` in the Canary's recovery kit"
    log_error "(canary-recovery-kit.json), or run this from a terminal to be asked for it."
    exit 1
  fi
  if [[ "$CANARY_TOKEN" == *[![:graph:]]* ]]; then
    log_error "CANARY_TOKEN holds a space or a non-printable character; it is not a Canary token."
    exit 1
  fi
}

# curl with the bearer header read from stdin, so the token never appears in
# curl's argv. printf is a shell builtin: no process sees it either. LAN
# only, so any proxy in the environment is ignored.
curl_auth() {
  printf 'Authorization: Bearer %s\n' "$CANARY_TOKEN" |
    curl --noproxy '*' -sS ${CURL_TLS[@]+"${CURL_TLS[@]}"} -H @- "$@"
}

# Lowercase 64-hex SHA-256 with colons/whitespace dropped, or nothing.
normalize_fp() {
  local fp
  fp=$(printf '%s' "$1" | tr -d ': \t\r\n' | tr 'A-F' 'a-f')
  if [[ "$fp" =~ ^[0-9a-f]{64}$ ]]; then
    printf '%s' "$fp"
  fi
}

# Does anything answer TLS on the HTTPS port? tls | plain (connection
# refused: an HTTP-only build) | unreachable:<curl exit>. Sends only an
# unauthenticated GET of a connectivity-probe path — never the token.
probe_tls() {
  local rc=0
  curl --noproxy '*' -sk -o /dev/null --connect-timeout 5 --max-time 10 \
    "https://${CANARY_IP}:${CANARY_HTTPS_PORT}/generate_204" || rc=$?
  case "$rc" in
    0) echo "tls" ;;
    7) echo "plain" ;;
    *) echo "unreachable:$rc" ;;
  esac
}

# The certificate the HTTPS port presents, PEM (empty on failure).
fetch_cert_pem() {
  openssl s_client -connect "${CANARY_IP}:${CANARY_HTTPS_PORT}" </dev/null 2>/dev/null |
    openssl x509 2>/dev/null || true
}

# SHA-256 of a PEM certificate's DER — what the device reports as tls_cert_fp.
cert_fp() {
  printf '%s\n' "$1" | openssl x509 -outform DER 2>/dev/null |
    openssl dgst -sha256 -r | cut -d' ' -f1
}

# curl's pin: base64 SHA-256 of the certificate's public key (SPKI).
cert_spki_pin() {
  printf '%s\n' "$1" | openssl x509 -pubkey -noout 2>/dev/null |
    openssl pkey -pubin -outform DER 2>/dev/null |
    openssl dgst -sha256 -binary | openssl base64 -A
}

# Decide how to reach the device: pinned HTTPS when CANARY_TLS_FP is set (no
# fallback), plain HTTP only when nothing listens on the HTTPS port.
resolve_transport() {
  local state pem presented spki
  state=$(probe_tls)
  if [[ -z "${CANARY_TLS_FP:-}" ]]; then
    case "$state" in
      plain)
        log_info "Nothing on port ${CANARY_HTTPS_PORT}: an HTTP-only build; using plain HTTP on port ${CANARY_PORT}"
        log_warn "Plain HTTP: the token and the image cross this link unencrypted."
        SCHEME="http"; PORT="$CANARY_PORT"
        return 0
        ;;
      tls)
        log_error "${CANARY_IP} serves HTTPS on port ${CANARY_HTTPS_PORT}, and CANARY_TLS_FP is not set."
        log_error "The token is only ever sent to a certificate you have pinned."
        if command -v openssl &>/dev/null; then
          pem=$(fetch_cert_pem)
          if [[ -n "$pem" ]]; then
            log_error "The device presented SHA-256 $(cert_fp "$pem")."
          fi
        fi
        log_error "If that equals \`tls_cert_fp\` in its recovery kit, set CANARY_TLS_FP to it and run again."
        exit 1
        ;;
      *)
        log_error "Cannot tell whether ${CANARY_IP} serves HTTPS on port ${CANARY_HTTPS_PORT} (curl exit ${state#unreachable:})."
        log_error "Check the address, or set CANARY_TLS_FP for a device that does."
        exit 1
        ;;
    esac
  fi

  PIN=$(normalize_fp "$CANARY_TLS_FP")
  if [[ -z "$PIN" ]]; then
    log_error "CANARY_TLS_FP is not a SHA-256 fingerprint (64 hex digits, colons allowed)."
    exit 1
  fi
  if [[ "$state" != "tls" ]]; then
    log_error "CANARY_TLS_FP is set but ${CANARY_IP}:${CANARY_HTTPS_PORT} does not answer TLS (${state}); not falling back to HTTP."
    exit 1
  fi
  if ! command -v openssl &>/dev/null; then
    log_error "openssl is required to check the device's certificate against CANARY_TLS_FP"
    exit 1
  fi
  pem=$(fetch_cert_pem)
  if [[ -z "$pem" ]]; then
    log_error "No TLS certificate from ${CANARY_IP}:${CANARY_HTTPS_PORT}."
    exit 1
  fi
  presented=$(cert_fp "$pem")
  if [[ "$presented" != "$PIN" ]]; then
    log_error "TLS certificate mismatch: presented ${presented:-none}, pinned ${PIN}."
    log_error "Refusing to send the token. A factory reset makes a new certificate;"
    log_error "re-read \`tls_cert_fp\` from the recovery kit if that is what happened."
    exit 1
  fi
  spki=$(cert_spki_pin "$pem")
  # The certificate is self-signed, so no CA can vouch for it: --insecure
  # drops the chain/hostname check and --pinnedpubkey replaces it — curl
  # aborts before sending a byte unless the server holds exactly the key of
  # the certificate whose fingerprint was just matched to the pin.
  CURL_TLS=(--insecure --pinnedpubkey "sha256//${spki}")
  SCHEME="https"; PORT="$CANARY_HTTPS_PORT"
  log_success "TLS certificate matches the pin"
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

# What a refusal means, in words.
explain_refusal() {
  case "$1" in
    301|302|307|308)
      log_error "The device redirects to HTTPS: set CANARY_TLS_FP to its \`tls_cert_fp\` (recovery kit) and run again." ;;
    401|403)
      log_error "The device refused the API token (HTTP $1). Use the recovery kit's \`token\`." ;;
    429)
      log_error "The device is rate-limiting after failed attempts (HTTP 429); wait and retry." ;;
    503)
      log_error "The device has no API token yet (HTTP 503): finish its setup first." ;;
  esac
}

# Deploy firmware via OTA
deploy_firmware() {
  local firmware_path="$1"
  local firmware_size
  firmware_size=$(stat -c %s "$firmware_path" 2>/dev/null || stat -f %z "$firmware_path" 2>/dev/null)

  log_info "Deploying: $firmware_path ($firmware_size bytes)"
  log_info "Target: ${SCHEME}://${CANARY_IP}:${PORT}${OTA_ENDPOINT}"

  local response
  local http_code

  response=$(curl_auth -w "\n%{http_code}" \
    --connect-timeout 10 \
    --max-time 120 \
    -X POST \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@${firmware_path}" \
    "${SCHEME}://${CANARY_IP}:${PORT}${OTA_ENDPOINT}" 2>&1) || true

  http_code=$(echo "$response" | tail -n1)
  response=$(echo "$response" | sed '$d')

  if [[ "$http_code" == "200" ]]; then
    log_success "OTA update successful!"
    log_info "Response: $response"
    log_info "Device will reboot..."
    return 0
  else
    log_error "OTA update failed (HTTP $http_code)"
    log_error "Response: $response"
    explain_refusal "$http_code"
    return 1
  fi
}

# GET /api/status with the token. Fails (stopping the deploy) when the token
# is refused, the device redirects, or — over HTTPS — the device reports a
# tls_cert_fp other than the pin.
check_device() {
  log_info "Checking device at ${SCHEME}://${CANARY_IP}:${PORT}..."

  local out code body reported
  if ! out=$(curl_auth -w "\n%{http_code}" --connect-timeout 5 --max-time 10 \
      "${SCHEME}://${CANARY_IP}:${PORT}${STATUS_ENDPOINT}" 2>/dev/null); then
    log_warn "Status endpoint not responding"
    log_warn "Continuing anyway (device may still accept OTA)"
    return 0
  fi
  code=$(echo "$out" | tail -n1)
  body=$(echo "$out" | sed '$d')

  case "$code" in
    200) ;;
    301|302|307|308|401|403|429|503)
      explain_refusal "$code"
      return 1
      ;;
    *)
      log_warn "Status endpoint answered HTTP ${code}; continuing anyway"
      return 0
      ;;
  esac

  if [[ "$SCHEME" == "https" ]]; then
    reported=$(printf '%s' "$body" |
      sed -n 's/.*"tls_cert_fp"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F:]*\)".*/\1/p' | head -n1)
    reported=$(normalize_fp "$reported")
    if [[ "$reported" != "$PIN" ]]; then
      log_error "The device reports tls_cert_fp ${reported:-none}, not the pinned ${PIN}."
      log_error "Refusing to deploy."
      return 1
    fi
  elif printf '%s' "$body" | grep -q '"tls_enabled"[[:space:]]*:[[:space:]]*true'; then
    log_error "The device reports TLS on; set CANARY_TLS_FP to its \`tls_cert_fp\` and run again."
    return 1
  fi
  log_success "Device is reachable and accepts the token"
  return 0
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
    fswatch -0 -e ".*" -i "\\.bin$" "$BUILD_DIR" | while read -r -d "" _path; do
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

  check_device || exit 1
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
      read_token
      resolve_transport
      check_device || exit 1
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
      echo "  CANARY_TOKEN       API token (recovery kit); asked for when unset. Never an argument."
      echo "  CANARY_TLS_FP      TLS pin: the device's tls_cert_fp (required for a TLS device)"
      echo "  CANARY_IP          Device IP (default: 192.168.4.1)"
      echo "  CANARY_PORT        HTTP port of an HTTP-only build (default: 80)"
      echo "  CANARY_HTTPS_PORT  HTTPS port (default: 443)"
      echo "  BUILD_DIR          PlatformIO build directory (default: .pio/build)"
      echo
      exit 0
      ;;
    *)
      read_token
      resolve_transport
      single_deploy
      ;;
  esac
}

main "$@"
