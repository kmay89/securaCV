#!/usr/bin/env bash
set -euo pipefail

# Safety gate: require explicit acknowledgement for irreversible eFuse burns.
ACK_FLAG=0
for arg in "$@"; do
  if [[ "$arg" == "--i-understand-this-is-irreversible" ]]; then
    ACK_FLAG=1
  fi
done
if [[ "$ACK_FLAG" -ne 1 ]]; then
  echo "ERROR: Refusing to run without explicit acknowledgement." >&2
  echo "       Re-run with: --i-understand-this-is-irreversible" >&2
  exit 2
fi

# ============================================================================
# SecuraCV Canary — Device Provisioning (Stages 2-4)
# ERRERlabs
#
# This script provisions a verified-virgin ESP32-S3 with:
#   - Signed Canary firmware (Secure Boot v2, RSA-3072)
#   - Flash Encryption (XTS-AES, release mode)
#   - JTAG disabled (pad + USB)
#   - UART download mode set to secure
#   - Per-device audit manifest
#
# Prerequisites:
#   - Keys generated via generate_keys.sh (Stage 0)
#   - Device verified via verify_device.py (Stage 1)
#   - ESP-IDF sourced in current shell
#   - Firmware built with security sdkconfig
#
# Usage:
#   ./provision_canary.sh --port /dev/ttyUSB0 [--firmware-dir ./build]
#   ./provision_canary.sh --port /dev/ttyACM0 --skip-build
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KEY_DIR="$PROJECT_ROOT/keys"
LOG_DIR="$PROJECT_ROOT/logs"
MANIFEST_DIR="$LOG_DIR/manifests"

# Default paths — override with flags
PORT=""
FIRMWARE_DIR=""
SKIP_BUILD=false
SKIP_VERIFY=false
DRY_RUN=false
OPERATOR="${USER:-unknown}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log() { echo -e "${CYAN}[PROV]${NC} $(date +%H:%M:%S) $1" | tee -a "$PROVISION_LOG"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $(date +%H:%M:%S) $1" | tee -a "$PROVISION_LOG"; }
err() { echo -e "${RED}[ERR]${NC} $(date +%H:%M:%S) $1" | tee -a "$PROVISION_LOG"; }
ok() { echo -e "${GREEN}[ OK ]${NC} $(date +%H:%M:%S) $1" | tee -a "$PROVISION_LOG"; }
step() { echo -e "\n${BOLD}── $1 ──${NC}" | tee -a "$PROVISION_LOG"; }

die() { err "$1"; exit 1; }

# ── Argument Parsing ────────────────────────────────────────────────────────

usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --port, -p PORT        Serial port (required, e.g., /dev/ttyUSB0)
  --firmware-dir DIR     Directory containing built firmware (default: auto-detect)
  --skip-build           Skip firmware build step (use existing build artifacts)
  --skip-verify          Skip virgin verification (DANGEROUS — use only for re-run)
  --dry-run              Show what would be done without executing eFuse burns
  --operator NAME        Operator name for audit log (default: \$USER)
  --help, -h             Show this help

Example:
  # Full provisioning flow
  ./provision_canary.sh --port /dev/ttyUSB0

  # Re-provision with existing build
  ./provision_canary.sh --port /dev/ttyUSB0 --skip-build --skip-verify

  # Preview without burning anything
  ./provision_canary.sh --port /dev/ttyUSB0 --dry-run
EOF
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --port|-p)      PORT="$2"; shift 2 ;;
        --firmware-dir) FIRMWARE_DIR="$2"; shift 2 ;;
        --skip-build)   SKIP_BUILD=true; shift ;;
        --skip-verify)  SKIP_VERIFY=true; shift ;;
        --dry-run)      DRY_RUN=true; shift ;;
        --operator)     OPERATOR="$2"; shift 2 ;;
        --help|-h)      usage; exit 0 ;;
        --i-understand-this-is-irreversible) shift ;;
        *)              die "Unknown option: $1. Use --help for usage." ;;
    esac
done

[[ -z "$PORT" ]] && { usage; die "Port is required."; }

# ── Setup ───────────────────────────────────────────────────────────────────

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
PROVISION_LOG="$LOG_DIR/provision_${TIMESTAMP}.log"
mkdir -p "$LOG_DIR" "$MANIFEST_DIR"

echo "# Provisioning log — $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$PROVISION_LOG"
echo "# Operator: $OPERATOR" >> "$PROVISION_LOG"
echo "# Port: $PORT" >> "$PROVISION_LOG"
echo "" >> "$PROVISION_LOG"

echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║  SecuraCV Canary — Device Provisioning                      ║${NC}"
echo -e "${BOLD}║  ERRERlabs                                                  ║${NC}"
echo -e "${BOLD}║                                                              ║${NC}"
echo -e "${BOLD}║  Port:     ${PORT}$(printf '%*s' $((43 - ${#PORT})) '')║${NC}"
echo -e "${BOLD}║  Operator: ${OPERATOR}$(printf '%*s' $((43 - ${#OPERATOR})) '')║${NC}"
echo -e "${BOLD}║  Mode:     $(if $DRY_RUN; then echo 'DRY RUN (no eFuse burns)   '; else echo 'PRODUCTION                 '; fi)                ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

# ── Preflight Checks ───────────────────────────────────────────────────────

step "PREFLIGHT"

# Check tools
for tool in esptool.py espefuse.py espsecure.py python3; do
    if command -v "$tool" &>/dev/null; then
        ok "$tool found: $(command -v "$tool")"
    else
        die "$tool not found. Source ESP-IDF: . \$IDF_PATH/export.sh"
    fi
done

# Check keys exist
if [[ ! -f "$KEY_DIR/secure_boot_signing_key_0.pem" ]]; then
    die "Signing key not found. Run generate_keys.sh first."
fi
if [[ ! -f "$KEY_DIR/flash_encryption_key.bin" ]]; then
    die "Flash encryption key not found. Run generate_keys.sh first."
fi
ok "Cryptographic keys present."

# Check serial port
if [[ ! -e "$PORT" ]]; then
    die "Serial port $PORT does not exist."
fi
ok "Serial port $PORT exists."

# ── Stage 1: Virgin Verification ───────────────────────────────────────────

step "STAGE 1: Device Verification"

if $SKIP_VERIFY; then
    warn "Skipping virgin verification (--skip-verify). This is DANGEROUS."
    warn "Only use this flag when re-running a failed provisioning on a known device."
else
    log "Running virgin state verification..."
    VERIFY_OUTPUT="$LOG_DIR/verify_${TIMESTAMP}.json"

    if ! python3 "$SCRIPT_DIR/verify_device.py" \
        --port "$PORT" \
        --output "$VERIFY_OUTPUT" 2>&1 | tee -a "$PROVISION_LOG"; then
        err ""
        err "DEVICE VERIFICATION FAILED."
        err "This device has non-default eFuse values and may be tampered."
        err "DO NOT PROVISION. Quarantine this device."
        err ""
        err "Verification details: $VERIFY_OUTPUT"
        exit 1
    fi
    ok "Device passed virgin verification."
fi

# ── Read device identity for manifest ───────────────────────────────────────

log "Reading device identity..."
CHIP_INFO=$(esptool.py --port "$PORT" --chip esp32s3 chip_id 2>&1 || true)
MAC_ADDR=$(echo "$CHIP_INFO" | grep -oP 'MAC:\s*\K[0-9a-f:]{17}' || echo "unknown")
CHIP_MODEL=$(echo "$CHIP_INFO" | grep -oP 'Chip is \K[^\s]+' || echo "unknown")
log "  MAC: $MAC_ADDR"
log "  Chip: $CHIP_MODEL"

# Sanitize MAC for filenames
MAC_SAFE=$(echo "$MAC_ADDR" | tr ':' '-')

# ── Stage 2: Build Firmware ────────────────────────────────────────────────

step "STAGE 2: Firmware Build"

if $SKIP_BUILD; then
    warn "Skipping firmware build (--skip-build)."
    if [[ -z "$FIRMWARE_DIR" ]]; then
        # Try to auto-detect
        for candidate in ./build ../canary-firmware/build ../build; do
            if [[ -d "$candidate" ]] && [[ -f "$candidate/bootloader/bootloader.bin" ]]; then
                FIRMWARE_DIR="$candidate"
                break
            fi
        done
    fi
    if [[ -z "$FIRMWARE_DIR" ]] || [[ ! -d "$FIRMWARE_DIR" ]]; then
        die "No firmware build directory found. Specify --firmware-dir or remove --skip-build."
    fi
    ok "Using existing build: $FIRMWARE_DIR"
else
    log "Building firmware with security configuration..."
    log "NOTE: Firmware should be built with the following sdkconfig settings:"
    log "  CONFIG_SECURE_BOOT=y"
    log "  CONFIG_SECURE_BOOT_V2_ENABLED=y"
    log "  CONFIG_SECURE_FLASH_ENC_ENABLED=y"
    log "  CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y"
    log "  CONFIG_BT_ENABLED=  (Bluetooth DISABLED)"
    log "  CONFIG_SECURE_BOOT_SIGNING_KEY=$KEY_DIR/secure_boot_signing_key_0.pem"
    log ""
    log "If using PlatformIO, ensure your platformio.ini includes:"
    log "  board_build.partitions = partitions_encrypted.csv"
    log "  board_build.flash_mode = dio"
    log ""
    warn "Automatic build integration depends on your project structure."
    warn "Please build manually and re-run with --skip-build --firmware-dir <path>"
    warn ""

    # If idf.py is available and we're in a project directory, try building
    if command -v idf.py &>/dev/null && [[ -f "CMakeLists.txt" ]]; then
        log "Detected ESP-IDF project. Building..."
        idf.py build 2>&1 | tee -a "$PROVISION_LOG"
        FIRMWARE_DIR="./build"
        ok "Build complete."
    else
        die "Cannot auto-build. Please build firmware and re-run with --skip-build."
    fi
fi

# Verify firmware artifacts exist
BOOTLOADER="$FIRMWARE_DIR/bootloader/bootloader.bin"
APP_BIN=$(find "$FIRMWARE_DIR" -maxdepth 1 -name "*.bin" -not -name "bootloader.bin" \
    -not -name "partition*" -not -name "ota*" | head -1)
PARTITION_TABLE="$FIRMWARE_DIR/partition_table/partition-table.bin"

# Fallback paths for PlatformIO
[[ ! -f "$BOOTLOADER" ]] && BOOTLOADER="$FIRMWARE_DIR/bootloader.bin"
[[ ! -f "$PARTITION_TABLE" ]] && PARTITION_TABLE="$FIRMWARE_DIR/partitions.bin"

for artifact in "$BOOTLOADER"; do
    if [[ ! -f "$artifact" ]]; then
        die "Firmware artifact not found: $artifact"
    fi
done
ok "Firmware artifacts located."
log "  Bootloader:      $BOOTLOADER"
log "  Application:     ${APP_BIN:-'(not found — will look for default)'}"
log "  Partition table: $PARTITION_TABLE"

# ── Stage 3: Flash & Provision ─────────────────────────────────────────────

step "STAGE 3: Flash & Provision (eFuse Burns)"

if $DRY_RUN; then
    warn "═══════════════════════════════════════════"
    warn "DRY RUN — The following commands WOULD execute:"
    warn "═══════════════════════════════════════════"
fi

# 3a: Burn Flash Encryption key
log "Step 3a: Burning flash encryption key to BLOCK_KEY0..."
EFUSE_CMD_FE=(
    espefuse.py --port "$PORT" --chip esp32s3
    burn_key BLOCK_KEY0
    "$KEY_DIR/flash_encryption_key.bin"
    XTS_AES_128_KEY
)
if $DRY_RUN; then
    log "  [DRY RUN] ${EFUSE_CMD_FE[*]}"
else
    log "  Executing: ${EFUSE_CMD_FE[*]}"
    echo "BURN" | "${EFUSE_CMD_FE[@]}" --do-not-confirm 2>&1 | tee -a "$PROVISION_LOG"
    ok "Flash encryption key burned to BLOCK_KEY0."
fi

# 3b: Burn Secure Boot key digest
log "Step 3b: Burning secure boot digest to BLOCK_KEY1..."
EFUSE_CMD_SB=(
    espefuse.py --port "$PORT" --chip esp32s3
    burn_key BLOCK_KEY1
    "$KEY_DIR/secure_boot_digest_0.bin"
    SECURE_BOOT_DIGEST0
)
if $DRY_RUN; then
    log "  [DRY RUN] ${EFUSE_CMD_SB[*]}"
else
    log "  Executing: ${EFUSE_CMD_SB[*]}"
    echo "BURN" | "${EFUSE_CMD_SB[@]}" --do-not-confirm 2>&1 | tee -a "$PROVISION_LOG"
    ok "Secure boot digest burned to BLOCK_KEY1."
fi

# 3c: Optionally burn rotation key (slot 1) to BLOCK_KEY2
if [[ -f "$KEY_DIR/secure_boot_digest_1.bin" ]]; then
    log "Step 3c: Burning rotation secure boot digest to BLOCK_KEY2..."
    EFUSE_CMD_SB2=(
        espefuse.py --port "$PORT" --chip esp32s3
        burn_key BLOCK_KEY2
        "$KEY_DIR/secure_boot_digest_1.bin"
        SECURE_BOOT_DIGEST1
    )
    if $DRY_RUN; then
        log "  [DRY RUN] ${EFUSE_CMD_SB2[*]}"
    else
        echo "BURN" | "${EFUSE_CMD_SB2[@]}" --do-not-confirm 2>&1 | tee -a "$PROVISION_LOG"
        ok "Rotation key digest burned to BLOCK_KEY2."
    fi
fi

# 3d: Flash firmware (pre-encrypted if needed)
log "Step 3d: Flashing firmware..."
FLASH_CMD=(
    esptool.py --port "$PORT" --chip esp32s3
    --baud 460800
    --before default_reset --after no_reset
    write_flash --flash_mode dio --flash_freq 80m --flash_size detect
)

# Add flash targets (addresses depend on partition layout)
FLASH_ARGS=()
FLASH_ARGS+=(0x0 "$BOOTLOADER")
[[ -f "$PARTITION_TABLE" ]] && FLASH_ARGS+=(0x8000 "$PARTITION_TABLE")
[[ -n "$APP_BIN" ]] && [[ -f "$APP_BIN" ]] && FLASH_ARGS+=(0x10000 "$APP_BIN")

if $DRY_RUN; then
    log "  [DRY RUN] ${FLASH_CMD[*]} ${FLASH_ARGS[*]}"
else
    log "  Flashing ${#FLASH_ARGS[@]} components..."
    "${FLASH_CMD[@]}" "${FLASH_ARGS[@]}" 2>&1 | tee -a "$PROVISION_LOG"
    ok "Firmware flashed."
fi

# 3e: Burn security eFuses
log "Step 3e: Burning security eFuses..."

# Collect all security eFuses to burn in one command
SECURITY_EFUSES=(
    SPI_BOOT_CRYPT_CNT 7
    SECURE_BOOT_EN 1
    DIS_PAD_JTAG 1
    DIS_USB_JTAG 1
    ENABLE_SECURITY_DOWNLOAD 1
    DIS_DOWNLOAD_MANUAL_ENCRYPT 1
)

EFUSE_CMD_SEC=(
    espefuse.py --port "$PORT" --chip esp32s3
    burn_efuse "${SECURITY_EFUSES[@]}"
)

if $DRY_RUN; then
    log "  [DRY RUN] ${EFUSE_CMD_SEC[*]}"
else
    echo ""
    echo -e "${RED}${BOLD}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}${BOLD}║  WARNING: IRREVERSIBLE EFUSE BURNS                       ║${NC}"
    echo -e "${RED}${BOLD}║                                                          ║${NC}"
    echo -e "${RED}${BOLD}║  The following eFuses will be permanently burned:         ║${NC}"
    echo -e "${RED}${BOLD}║    • SPI_BOOT_CRYPT_CNT = 7 (flash encryption release)   ║${NC}"
    echo -e "${RED}${BOLD}║    • SECURE_BOOT_EN = 1 (enable secure boot v2)          ║${NC}"
    echo -e "${RED}${BOLD}║    • DIS_PAD_JTAG = 1 (disable JTAG pads)               ║${NC}"
    echo -e "${RED}${BOLD}║    • DIS_USB_JTAG = 1 (disable USB JTAG)                ║${NC}"
    echo -e "${RED}${BOLD}║    • ENABLE_SECURITY_DOWNLOAD = 1 (secure UART mode)    ║${NC}"
    echo -e "${RED}${BOLD}║    • DIS_DOWNLOAD_MANUAL_ENCRYPT = 1                    ║${NC}"
    echo -e "${RED}${BOLD}║                                                          ║${NC}"
    echo -e "${RED}${BOLD}║  THIS CANNOT BE UNDONE. THE DEVICE IS PERMANENTLY        ║${NC}"
    echo -e "${RED}${BOLD}║  LOCKED TO THIS SECURITY CONFIGURATION.                  ║${NC}"
    echo -e "${RED}${BOLD}╚══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    read -p "Type 'BURN' to proceed: " CONFIRM
    if [[ "$CONFIRM" != "BURN" ]]; then
        warn "Aborted by operator. Device is partially provisioned."
        warn "Keys are burned but security eFuses are not set."
        warn "Re-run with --skip-verify to complete provisioning."
        exit 1
    fi

    log "  Executing: ${EFUSE_CMD_SEC[*]}"
    echo "BURN" | "${EFUSE_CMD_SEC[@]}" --do-not-confirm 2>&1 | tee -a "$PROVISION_LOG"
    ok "Security eFuses burned."
fi

# ── Stage 4: Post-Provisioning Verification ────────────────────────────────

step "STAGE 4: Post-Provisioning Verification"

if $DRY_RUN; then
    warn "[DRY RUN] Skipping post-provisioning verification."
else
    log "Resetting device..."
    esptool.py --port "$PORT" --chip esp32s3 --after hard_reset read_mac 2>&1 | tee -a "$PROVISION_LOG" || true
    sleep 3

    log "Running post-provisioning verification..."
    POST_VERIFY="$LOG_DIR/verify_post_${TIMESTAMP}.json"
    if python3 "$SCRIPT_DIR/verify_device.py" \
        --port "$PORT" \
        --post-provision \
        --output "$POST_VERIFY" 2>&1 | tee -a "$PROVISION_LOG"; then
        ok "Post-provisioning verification PASSED."
    else
        err "Post-provisioning verification FAILED."
        err "Device may be in an inconsistent state."
        err "Check: $POST_VERIFY"
    fi
fi

# ── Stage 5: Generate Manifest ─────────────────────────────────────────────

step "STAGE 5: Device Manifest"

MANIFEST_FILE="$MANIFEST_DIR/canary_${MAC_SAFE}_${TIMESTAMP}.json"

# Compute firmware hashes
BL_HASH=$(sha256sum "$BOOTLOADER" 2>/dev/null | cut -d' ' -f1 || echo "unknown")
APP_HASH=""
[[ -n "$APP_BIN" ]] && [[ -f "$APP_BIN" ]] && \
    APP_HASH=$(sha256sum "$APP_BIN" | cut -d' ' -f1)
PT_HASH=""
[[ -f "$PARTITION_TABLE" ]] && \
    PT_HASH=$(sha256sum "$PARTITION_TABLE" | cut -d' ' -f1)

cat > "$MANIFEST_FILE" << MANIFEST_EOF
{
  "schema_version": "1.0",
  "product": "SecuraCV Canary",
  "manufacturer": "ERRERlabs",
  "provisioning": {
    "timestamp_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "operator": "$OPERATOR",
    "host": "$(hostname)",
    "dry_run": $DRY_RUN,
    "log_file": "$(basename "$PROVISION_LOG")"
  },
  "device": {
    "chip_model": "$CHIP_MODEL",
    "mac_address": "$MAC_ADDR",
    "serial_port": "$PORT"
  },
  "security": {
    "secure_boot": "v2_rsa3072",
    "flash_encryption": "xts_aes_128",
    "signing_key_slot": 0,
    "signing_key_file_hash": "$(sha256sum "$KEY_DIR/secure_boot_signing_key_0.pem" 2>/dev/null | cut -d' ' -f1 || echo 'unknown')",
    "rotation_key_provisioned": $(if [[ -f "$KEY_DIR/secure_boot_digest_1.bin" ]]; then echo "true"; else echo "false"; fi),
    "jtag_disabled": true,
    "uart_download_secure": true,
    "bluetooth_compiled": false
  },
  "firmware": {
    "bootloader_sha256": "$BL_HASH",
    "application_sha256": "${APP_HASH:-null}",
    "partition_table_sha256": "${PT_HASH:-null}",
    "bootloader_path": "$BOOTLOADER",
    "application_path": "${APP_BIN:-null}"
  }
}
MANIFEST_EOF

ok "Device manifest: $MANIFEST_FILE"

# ── Done ────────────────────────────────────────────────────────────────────

echo ""
if $DRY_RUN; then
    echo -e "${YELLOW}${BOLD}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${YELLOW}${BOLD}║  DRY RUN COMPLETE                                        ║${NC}"
    echo -e "${YELLOW}${BOLD}║  No eFuses were burned. No firmware was flashed.          ║${NC}"
    echo -e "${YELLOW}${BOLD}║  Remove --dry-run to execute for real.                    ║${NC}"
    echo -e "${YELLOW}${BOLD}╚══════════════════════════════════════════════════════════╝${NC}"
else
    echo -e "${GREEN}${BOLD}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}${BOLD}║  PROVISIONING COMPLETE                                    ║${NC}"
    echo -e "${GREEN}${BOLD}║                                                            ║${NC}"
    echo -e "${GREEN}${BOLD}║  Device: $MAC_ADDR$(printf '%*s' $((35 - ${#MAC_ADDR})) '')║${NC}"
    echo -e "${GREEN}${BOLD}║  Secure Boot v2:       ENABLED                             ║${NC}"
    echo -e "${GREEN}${BOLD}║  Flash Encryption:     ENABLED (release)                   ║${NC}"
    echo -e "${GREEN}${BOLD}║  JTAG:                 DISABLED                            ║${NC}"
    echo -e "${GREEN}${BOLD}║  Bluetooth:            NOT COMPILED                        ║${NC}"
    echo -e "${GREEN}${BOLD}║                                                            ║${NC}"
    echo -e "${GREEN}${BOLD}║  Manifest: $(basename "$MANIFEST_FILE")${NC}"
    echo -e "${GREEN}${BOLD}║  Log:      $(basename "$PROVISION_LOG")${NC}"
    echo -e "${GREEN}${BOLD}╚══════════════════════════════════════════════════════════╝${NC}"
fi
echo ""
log "Provisioning session complete."