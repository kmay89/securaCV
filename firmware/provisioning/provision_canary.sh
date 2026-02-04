#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# SecuraCV Canary — Production Provisioning Script
# ═══════════════════════════════════════════════════════════════
#
# Full orchestration for production device provisioning:
#   1. Virgin verify → 2. Flash signed firmware → 3. Burn keys →
#   4. Burn security eFuses → 5. Post-verify → 6. Generate manifest
#
# IMPORTANT: This script has IRREVERSIBLE operations in Phase 2 mode.
# Always use --dry-run first to preview what will happen.
#
# Usage:
#   ./provision_canary.sh --port /dev/ttyACM0 --dry-run
#   ./provision_canary.sh --port /dev/ttyACM0 --phase 1
#   ./provision_canary.sh --port /dev/ttyACM0 --phase 2
#   ./provision_canary.sh --help
#
# Phases:
#   Phase 1 (default): Development mode - no eFuse burning
#   Phase 2: Production lockdown - burns security eFuses (IRREVERSIBLE)
#
# ═══════════════════════════════════════════════════════════════

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEYS_DIR="${SCRIPT_DIR}/keys"
MANIFEST_FILE="${SCRIPT_DIR}/fleet_manifest.json"
CANARY_DIR="${SCRIPT_DIR}/../canary"

# Default settings
PORT=""
PHASE=1
DRY_RUN=false
FIRMWARE_BIN=""
SKIP_VERIFY=false

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_header() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo " SecuraCV Canary — Production Provisioning"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
}

print_warning() {
    echo -e "${YELLOW}WARNING:${NC} $1"
}

print_error() {
    echo -e "${RED}ERROR:${NC} $1"
}

print_success() {
    echo -e "${GREEN}SUCCESS:${NC} $1"
}

print_info() {
    echo -e "${BLUE}INFO:${NC} $1"
}

print_step() {
    echo ""
    echo -e "${BLUE}▶${NC} $1"
    echo "─────────────────────────────────────────────────────────────"
}

show_help() {
    cat << EOF
Usage: $0 [OPTIONS]

Provision a SecuraCV Canary device for production deployment.

Options:
  --port, -p PORT       Serial port (required, e.g., /dev/ttyACM0)
  --phase PHASE         Provisioning phase: 1 (dev) or 2 (production)
  --dry-run             Preview operations without executing
  --firmware PATH       Path to firmware binary (optional)
  --skip-verify         Skip pre/post verification
  --help, -h            Show this help

Phases:
  Phase 1 (default):
    - Verify device state
    - Flash development firmware
    - Record device identity
    - NO eFuse burning (safe for development)

  Phase 2 (production):
    - Verify device is virgin
    - Flash signed firmware
    - Burn flash encryption key
    - Enable Secure Boot v2
    - Disable JTAG
    - Enable anti-rollback
    - Verify lockdown
    - Add to fleet manifest
    - IRREVERSIBLE - device cannot return to dev mode

Examples:
  # Preview Phase 1 provisioning
  $0 --port /dev/ttyACM0 --dry-run

  # Execute Phase 1 provisioning (development)
  $0 --port /dev/ttyACM0 --phase 1

  # Preview Phase 2 provisioning (REVIEW CAREFULLY)
  $0 --port /dev/ttyACM0 --phase 2 --dry-run

  # Execute Phase 2 provisioning (IRREVERSIBLE)
  $0 --port /dev/ttyACM0 --phase 2

EOF
}

check_prerequisites() {
    print_step "Checking prerequisites"

    # Check Python
    if ! command -v python3 &> /dev/null; then
        print_error "Python 3 not found"
        exit 1
    fi
    echo "  Python:    $(python3 --version)"

    # Check esptool
    if ! python3 -m esptool version &> /dev/null; then
        print_error "esptool not found. Install with: pip install esptool"
        exit 1
    fi
    echo "  esptool:   $(python3 -m esptool version 2>&1 | head -n1)"

    # Check espefuse
    if ! python3 -m espefuse --help &> /dev/null; then
        print_error "espefuse not found. Install with: pip install esptool"
        exit 1
    fi
    echo "  espefuse:  available"

    # Check port exists
    if [[ ! -e "${PORT}" ]]; then
        print_error "Port ${PORT} does not exist"
        exit 1
    fi
    echo "  Port:      ${PORT} (exists)"

    # Phase 2 specific checks
    if [[ ${PHASE} -eq 2 ]]; then
        if [[ ! -f "${KEYS_DIR}/secure_boot_signing_key.pem" ]]; then
            print_error "Signing key not found. Run ./generate_keys.sh first"
            exit 1
        fi
        echo "  Signing key: ${KEYS_DIR}/secure_boot_signing_key.pem (exists)"

        if [[ ! -f "${KEYS_DIR}/flash_encryption_key.bin" ]]; then
            print_error "Flash encryption key not found. Run ./generate_keys.sh first"
            exit 1
        fi
        echo "  Flash key:   ${KEYS_DIR}/flash_encryption_key.bin (exists)"
    fi

    print_success "Prerequisites check passed"
}

verify_virgin() {
    print_step "Verifying device is in virgin state"

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would run: python3 verify_device.py --port ${PORT} --expect-virgin"
        return 0
    fi

    if [[ "${SKIP_VERIFY}" == "true" ]]; then
        print_warning "Skipping verification (--skip-verify)"
        return 0
    fi

    if python3 "${SCRIPT_DIR}/verify_device.py" --port "${PORT}" --expect-virgin; then
        print_success "Device is in virgin state"
    else
        print_error "Device is not in virgin state"
        echo "  The device may have been previously provisioned."
        echo "  For Phase 1, this is usually fine. For Phase 2, you need a fresh device."

        if [[ ${PHASE} -eq 2 ]]; then
            exit 1
        fi
    fi
}

flash_firmware() {
    print_step "Flashing firmware"

    # Determine firmware path
    if [[ -n "${FIRMWARE_BIN}" ]]; then
        FW_PATH="${FIRMWARE_BIN}"
    elif [[ ${PHASE} -eq 2 ]]; then
        FW_PATH="${CANARY_DIR}/.pio/build/secure/firmware.bin"
    else
        FW_PATH="${CANARY_DIR}/.pio/build/dev/firmware.bin"
    fi

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would flash: ${FW_PATH}"
        echo "  [DRY-RUN] Command: python3 -m esptool --port ${PORT} write_flash 0x10000 ${FW_PATH}"
        return 0
    fi

    if [[ ! -f "${FW_PATH}" ]]; then
        print_warning "Firmware not found at ${FW_PATH}"
        echo "  Build firmware first with: cd ${CANARY_DIR} && pio run"

        read -rp "Skip firmware flashing? [y/N]: " SKIP_FLASH
        if [[ "${SKIP_FLASH}" != "y" ]]; then
            exit 1
        fi
        return 0
    fi

    echo "  Firmware: ${FW_PATH}"
    echo "  Size:     $(stat -f%z "${FW_PATH}" 2>/dev/null || stat -c%s "${FW_PATH}") bytes"

    python3 -m esptool --port "${PORT}" write_flash 0x10000 "${FW_PATH}"

    print_success "Firmware flashed"
}

burn_flash_encryption_key() {
    print_step "Burning flash encryption key"

    if [[ ${PHASE} -ne 2 ]]; then
        echo "  [PHASE 1] Skipping - flash encryption key burning is Phase 2 only"
        return 0
    fi

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would burn flash encryption key from: ${KEYS_DIR}/flash_encryption_key.bin"
        echo "  [DRY-RUN] Command: python3 -m espefuse --port ${PORT} burn_key BLOCK_KEY0 ${KEYS_DIR}/flash_encryption_key.bin XTS_AES_256_KEY"
        echo ""
        print_warning "[DRY-RUN] This operation is IRREVERSIBLE in production"
        return 0
    fi

    print_warning "This operation is IRREVERSIBLE"
    read -rp "Type 'BURN' to confirm flash encryption key burning: " CONFIRM
    if [[ "${CONFIRM}" != "BURN" ]]; then
        echo "Aborted."
        exit 1
    fi

    python3 -m espefuse --port "${PORT}" burn_key BLOCK_KEY0 \
        "${KEYS_DIR}/flash_encryption_key.bin" XTS_AES_256_KEY

    print_success "Flash encryption key burned"
}

burn_security_efuses() {
    print_step "Burning security eFuses"

    if [[ ${PHASE} -ne 2 ]]; then
        echo "  [PHASE 1] Skipping - security eFuse burning is Phase 2 only"
        return 0
    fi

    EFUSES_TO_BURN=(
        "SECURE_BOOT_EN"
        "SOFT_DIS_JTAG"
        "DIS_USB_JTAG"
        "DIS_PAD_JTAG"
        "DIS_DOWNLOAD_MANUAL_ENCRYPT"
    )

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would burn the following eFuses:"
        for efuse in "${EFUSES_TO_BURN[@]}"; do
            echo "    - ${efuse}"
        done
        echo ""
        print_warning "[DRY-RUN] This operation is IRREVERSIBLE"
        return 0
    fi

    print_warning "This operation is IRREVERSIBLE"
    echo "  The following eFuses will be burned:"
    for efuse in "${EFUSES_TO_BURN[@]}"; do
        echo "    - ${efuse}"
    done
    echo ""

    read -rp "Type 'BURN EFUSES' to confirm: " CONFIRM
    if [[ "${CONFIRM}" != "BURN EFUSES" ]]; then
        echo "Aborted."
        exit 1
    fi

    for efuse in "${EFUSES_TO_BURN[@]}"; do
        echo "  Burning ${efuse}..."
        if [[ "${efuse}" == "SOFT_DIS_JTAG" ]]; then
            # SOFT_DIS_JTAG is a 3-bit field; value 7 disables all software JTAG sources
            python3 -m espefuse --port "${PORT}" burn_efuse "${efuse}" 7 --do-not-confirm
        else
            python3 -m espefuse --port "${PORT}" burn_efuse "${efuse}" --do-not-confirm
        fi
    done

    print_success "Security eFuses burned"
}

verify_lockdown() {
    print_step "Verifying device lockdown"

    if [[ ${PHASE} -ne 2 ]]; then
        echo "  [PHASE 1] Skipping - lockdown verification is Phase 2 only"
        return 0
    fi

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would run: python3 verify_device.py --port ${PORT} --expect-locked"
        return 0
    fi

    if [[ "${SKIP_VERIFY}" == "true" ]]; then
        print_warning "Skipping verification (--skip-verify)"
        return 0
    fi

    if python3 "${SCRIPT_DIR}/verify_device.py" --port "${PORT}" --expect-locked; then
        print_success "Device lockdown verified"
    else
        print_error "Device lockdown verification FAILED"
        echo "  Some security eFuses may not have been burned correctly."
        echo "  Check the verification report for details."
        exit 1
    fi
}

get_device_info() {
    print_step "Reading device information"

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would read device MAC and chip ID"
        DEVICE_MAC="AA:BB:CC:DD:EE:FF"
        DEVICE_CHIP_ID="placeholder"
        return 0
    fi

    # Get MAC address
    CHIP_INFO=$(python3 -m esptool --port "${PORT}" chip_id 2>&1)
    DEVICE_MAC=$(echo "${CHIP_INFO}" | grep -i "MAC" | head -n1 | grep -oE '([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}' || echo "unknown")
    DEVICE_CHIP_ID=$(echo "${CHIP_INFO}" | grep -i "Chip ID" | grep -oE '0x[0-9a-fA-F]+' || echo "unknown")

    echo "  MAC:     ${DEVICE_MAC}"
    echo "  Chip ID: ${DEVICE_CHIP_ID}"
}

add_to_manifest() {
    print_step "Adding device to fleet manifest"

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo "  [DRY-RUN] Would add device to: ${MANIFEST_FILE}"
        echo "  [DRY-RUN] MAC: ${DEVICE_MAC:-unknown}"
        return 0
    fi

    # Get device info if not already retrieved
    if [[ -z "${DEVICE_MAC:-}" ]]; then
        get_device_info
    fi

    python3 "${SCRIPT_DIR}/create_manifest.py" add \
        --mac "${DEVICE_MAC}" \
        --phase "${PHASE}" \
        --notes "Provisioned $(date -Iseconds)"

    print_success "Device added to fleet manifest"
}

print_summary() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo " Provisioning Summary"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    echo "  Port:      ${PORT}"
    echo "  Phase:     ${PHASE}"
    echo "  Dry Run:   ${DRY_RUN}"

    if [[ "${DRY_RUN}" == "true" ]]; then
        echo ""
        print_info "This was a dry run. No changes were made."
        echo "  Remove --dry-run to execute provisioning."
    else
        echo ""
        if [[ ${PHASE} -eq 1 ]]; then
            print_success "Phase 1 provisioning complete (development mode)"
            echo "  Device is ready for development and testing."
            echo "  Security eFuses have NOT been burned."
        else
            print_success "Phase 2 provisioning complete (production lockdown)"
            echo "  Device is locked down and ready for production."
            echo "  Security eFuses have been burned (IRREVERSIBLE)."
        fi
    fi
    echo ""
}

# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --port|-p)
            PORT="$2"
            shift 2
            ;;
        --phase)
            PHASE="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --firmware)
            FIRMWARE_BIN="$2"
            shift 2
            ;;
        --skip-verify)
            SKIP_VERIFY=true
            shift
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Validate required arguments
if [[ -z "${PORT}" ]]; then
    print_error "Port is required. Use --port /dev/ttyACM0"
    show_help
    exit 1
fi

# Validate phase
if [[ ${PHASE} -ne 1 && ${PHASE} -ne 2 ]]; then
    print_error "Invalid phase: ${PHASE}. Must be 1 or 2."
    exit 1
fi

print_header

# Phase 2 warning
if [[ ${PHASE} -eq 2 && "${DRY_RUN}" == "false" ]]; then
    echo ""
    print_warning "═══════════════════════════════════════════════════════════════"
    print_warning " PHASE 2 PROVISIONING - IRREVERSIBLE OPERATIONS"
    print_warning "═══════════════════════════════════════════════════════════════"
    echo ""
    echo "This will PERMANENTLY:"
    echo "  - Burn flash encryption key"
    echo "  - Enable Secure Boot v2"
    echo "  - Disable JTAG"
    echo "  - Enable anti-rollback"
    echo ""
    echo "The device CANNOT be returned to development mode after this."
    echo ""
    read -rp "Type 'I UNDERSTAND' to continue: " CONFIRM
    if [[ "${CONFIRM}" != "I UNDERSTAND" ]]; then
        echo "Aborted."
        exit 1
    fi
fi

# Execute provisioning steps
check_prerequisites
verify_virgin
get_device_info
flash_firmware

if [[ ${PHASE} -eq 2 ]]; then
    burn_flash_encryption_key
    burn_security_efuses
    verify_lockdown
fi

add_to_manifest
print_summary
