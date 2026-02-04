#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# SecuraCV Canary — Secure Key Generation Script
# ═══════════════════════════════════════════════════════════════
#
# Generates RSA-3072 signing key + XTS-AES-256 flash encryption key
# with entropy validation and backup checklist.
#
# Usage:
#   ./generate_keys.sh              # Interactive mode with prompts
#   ./generate_keys.sh --batch      # Non-interactive (CI/CD)
#   ./generate_keys.sh --help       # Show this help
#
# Output:
#   keys/secure_boot_signing_key.pem    - RSA-3072 private key (KEEP SECRET)
#   keys/secure_boot_signing_key.pub    - RSA-3072 public key
#   keys/flash_encryption_key.bin       - 256-bit AES key (KEEP SECRET)
#   keys/key_inventory.json             - Metadata for tracking
#
# IMPORTANT: These keys are used for Phase 2 (production lockdown).
# Phase 1 (development) does not require these keys.
#
# ═══════════════════════════════════════════════════════════════

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEYS_DIR="${SCRIPT_DIR}/keys"
BATCH_MODE=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_header() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo " SecuraCV Canary — Secure Key Generation"
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

check_prerequisites() {
    echo "Checking prerequisites..."

    # Check OpenSSL
    if ! command -v openssl &> /dev/null; then
        print_error "OpenSSL not found. Install with: apt install openssl"
        exit 1
    fi

    OPENSSL_VERSION=$(openssl version | awk '{print $2}')
    echo "  OpenSSL version: ${OPENSSL_VERSION}"

    # Check for hardware RNG (optional)
    if [[ -c /dev/hwrng ]]; then
        echo "  Hardware RNG: Available (/dev/hwrng)"
        RANDOM_SOURCE="/dev/hwrng"
    elif [[ -c /dev/random ]]; then
        echo "  Hardware RNG: Fallback to /dev/random"
        RANDOM_SOURCE="/dev/random"
    else
        print_warning "No hardware RNG found. Using /dev/urandom (less entropy)"
        RANDOM_SOURCE="/dev/urandom"
    fi

    echo ""
}

check_entropy() {
    echo "Checking system entropy..."

    if [[ -f /proc/sys/kernel/random/entropy_avail ]]; then
        ENTROPY=$(cat /proc/sys/kernel/random/entropy_avail)
        echo "  Available entropy: ${ENTROPY} bits"

        if [[ ${ENTROPY} -lt 256 ]]; then
            print_warning "Low entropy (${ENTROPY} < 256 bits)"
            echo "  Generating entropy... (move mouse, type randomly, etc.)"

            if [[ "${BATCH_MODE}" == "false" ]]; then
                echo "  Press Enter when ready, or Ctrl+C to abort..."
                read -r
            else
                # In batch mode, wait for entropy to accumulate
                echo "  Waiting for entropy to accumulate..."
                sleep 10
            fi

            ENTROPY=$(cat /proc/sys/kernel/random/entropy_avail)
            if [[ ${ENTROPY} -lt 128 ]]; then
                print_error "Entropy still too low (${ENTROPY} < 128 bits). Aborting."
                exit 1
            fi
        fi

        print_success "Entropy check passed (${ENTROPY} bits)"
    else
        print_warning "Cannot check entropy (non-Linux system)"
    fi

    echo ""
}

check_existing_keys() {
    if [[ -f "${KEYS_DIR}/secure_boot_signing_key.pem" ]]; then
        print_warning "Existing signing key found at ${KEYS_DIR}/secure_boot_signing_key.pem"

        if [[ "${BATCH_MODE}" == "true" ]]; then
            print_error "In batch mode, refusing to overwrite existing keys."
            exit 1
        fi

        echo ""
        echo "Options:"
        echo "  1. Abort (recommended - use existing keys)"
        echo "  2. Backup existing keys and generate new ones"
        echo "  3. Overwrite (DANGEROUS - will break existing devices)"
        echo ""
        read -rp "Choice [1]: " CHOICE
        CHOICE=${CHOICE:-1}

        case ${CHOICE} in
            1)
                echo "Aborting. Use existing keys."
                exit 0
                ;;
            2)
                BACKUP_DIR="${KEYS_DIR}/backup_$(date +%Y%m%d_%H%M%S)"
                mkdir -p "${BACKUP_DIR}"
                mv "${KEYS_DIR}"/*.pem "${KEYS_DIR}"/*.pub "${KEYS_DIR}"/*.bin "${KEYS_DIR}"/*.json "${BACKUP_DIR}/" 2>/dev/null || true
                print_success "Existing keys backed up to ${BACKUP_DIR}"
                ;;
            3)
                print_warning "Overwriting existing keys. This will break existing devices!"
                read -rp "Type 'OVERWRITE' to confirm: " CONFIRM
                if [[ "${CONFIRM}" != "OVERWRITE" ]]; then
                    echo "Aborting."
                    exit 1
                fi
                ;;
            *)
                echo "Invalid choice. Aborting."
                exit 1
                ;;
        esac
    fi
}

generate_signing_key() {
    echo "Generating RSA-3072 signing key..."

    openssl genrsa -out "${KEYS_DIR}/secure_boot_signing_key.pem" 3072

    # Extract public key
    openssl rsa -in "${KEYS_DIR}/secure_boot_signing_key.pem" \
        -pubout -out "${KEYS_DIR}/secure_boot_signing_key.pub"

    # Set restrictive permissions
    chmod 600 "${KEYS_DIR}/secure_boot_signing_key.pem"
    chmod 644 "${KEYS_DIR}/secure_boot_signing_key.pub"

    # Calculate fingerprint
    FINGERPRINT=$(openssl rsa -in "${KEYS_DIR}/secure_boot_signing_key.pem" -pubout -outform DER | \
        openssl dgst -sha256 -binary | head -c 8 | xxd -p)

    print_success "Signing key generated (fingerprint: ${FINGERPRINT})"
    echo "  Private key: ${KEYS_DIR}/secure_boot_signing_key.pem"
    echo "  Public key:  ${KEYS_DIR}/secure_boot_signing_key.pub"
    echo ""
}

generate_flash_encryption_key() {
    echo "Generating XTS-AES-256 flash encryption key..."

    # ESP32-S3 uses 256-bit key for XTS-AES
    dd if="${RANDOM_SOURCE}" of="${KEYS_DIR}/flash_encryption_key.bin" bs=32 count=1

    # Set restrictive permissions
    chmod 600 "${KEYS_DIR}/flash_encryption_key.bin"

    # Calculate key hash for verification
    KEY_HASH=$(sha256sum "${KEYS_DIR}/flash_encryption_key.bin" | awk '{print $1}')

    print_success "Flash encryption key generated"
    echo "  Key file: ${KEYS_DIR}/flash_encryption_key.bin"
    echo "  SHA256:   ${KEY_HASH}"
    echo ""
}

generate_inventory() {
    echo "Generating key inventory..."

    SIGNING_FINGERPRINT=$(openssl rsa -in "${KEYS_DIR}/secure_boot_signing_key.pem" -pubout -outform DER | \
        openssl dgst -sha256 -binary | head -c 8 | xxd -p)
    ENCRYPTION_KEY_HASH=$(sha256sum "${KEYS_DIR}/flash_encryption_key.bin" | awk '{print $1}')

    cat > "${KEYS_DIR}/key_inventory.json" << EOF
{
  "version": 1,
  "generated_at": "$(date -Iseconds)",
  "generated_by": "$(whoami)@$(hostname)",
  "signing_key": {
    "algorithm": "RSA-3072",
    "file": "secure_boot_signing_key.pem",
    "public_file": "secure_boot_signing_key.pub",
    "fingerprint": "${SIGNING_FINGERPRINT}",
    "purpose": "Secure Boot v2 firmware signing"
  },
  "flash_encryption_key": {
    "algorithm": "XTS-AES-256",
    "file": "flash_encryption_key.bin",
    "sha256": "${ENCRYPTION_KEY_HASH}",
    "purpose": "ESP32-S3 flash encryption"
  },
  "notes": [
    "KEEP THESE FILES SECRET AND BACKED UP OFFLINE",
    "Loss of signing key = bricked devices on next OTA",
    "One flash encryption key can be used for a batch of devices",
    "Phase 2 (production lockdown) uses these keys",
    "Phase 1 (development) does not require these keys"
  ],
  "backup_checklist": {
    "offline_usb": false,
    "secure_vault": false,
    "printed_recovery": false,
    "verified_restore": false
  }
}
EOF

    print_success "Key inventory generated: ${KEYS_DIR}/key_inventory.json"
    echo ""
}

print_backup_checklist() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo " CRITICAL: Backup Checklist"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    echo " Before using these keys for production, complete this checklist:"
    echo ""
    echo " [ ] Copy keys/ directory to offline USB drive"
    echo " [ ] Store USB in secure vault / safe deposit box"
    echo " [ ] Print key_inventory.json for disaster recovery"
    echo " [ ] Test restore from backup on a separate machine"
    echo ""
    echo " Update backup_checklist in key_inventory.json as you complete each."
    echo ""
    print_warning "Loss of signing key = bricked devices on next OTA update"
    echo ""
}

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Generate RSA-3072 signing key and XTS-AES-256 flash encryption key"
    echo "for SecuraCV Canary production provisioning (Phase 2)."
    echo ""
    echo "Options:"
    echo "  --batch     Non-interactive mode (for CI/CD)"
    echo "  --help      Show this help"
    echo ""
    echo "Output files (in keys/ directory):"
    echo "  secure_boot_signing_key.pem  - Private key (KEEP SECRET)"
    echo "  secure_boot_signing_key.pub  - Public key"
    echo "  flash_encryption_key.bin     - Flash encryption key (KEEP SECRET)"
    echo "  key_inventory.json           - Metadata and backup checklist"
    echo ""
}

# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --batch)
            BATCH_MODE=true
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

print_header
check_prerequisites
check_entropy
mkdir -p "${KEYS_DIR}"
check_existing_keys
generate_signing_key
generate_flash_encryption_key
generate_inventory

if [[ "${BATCH_MODE}" == "false" ]]; then
    print_backup_checklist
fi

print_success "Key generation complete!"
echo ""
echo "Next steps:"
echo "  1. Complete the backup checklist above"
echo "  2. Use './provision_canary.sh --dry-run' to preview provisioning"
echo "  3. Read docs/secure_provisioning.md for full workflow"
echo ""
