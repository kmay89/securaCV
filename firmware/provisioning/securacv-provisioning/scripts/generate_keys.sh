#!/usr/bin/env bash
# ============================================================================
# SecuraCV Canary — Key Generation (Stage 0)
# ERRERlabs — Run ONCE per product line on a trusted workstation
# ============================================================================
#
# This script generates:
#   1. RSA-3072 Secure Boot v2 signing key pair
#   2. 256-bit XTS-AES flash encryption key
#   3. Optional: additional signing keys for key rotation slots (up to 3)
#
# SECURITY NOTES:
#   - Run on an air-gapped or trusted machine with good entropy
#   - Keys NEVER leave this machine (or HSM) except as digests
#   - NEVER commit keys/ directory to version control
#   - Back up keys to encrypted offline storage immediately
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KEY_DIR="$PROJECT_ROOT/keys"
LOG_FILE="$PROJECT_ROOT/logs/keygen_$(date +%Y%m%d_%H%M%S).log"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${CYAN}[KEYGEN]${NC} $1" | tee -a "$LOG_FILE"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1" | tee -a "$LOG_FILE"; }
err() { echo -e "${RED}[ERROR]${NC} $1" | tee -a "$LOG_FILE"; }
ok() { echo -e "${GREEN}[OK]${NC} $1" | tee -a "$LOG_FILE"; }

# ── Preflight ───────────────────────────────────────────────────────────────

mkdir -p "$KEY_DIR" "$PROJECT_ROOT/logs"

echo "# Key generation log — $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$LOG_FILE"
echo "# Host: $(hostname)" >> "$LOG_FILE"
echo "# User: $(whoami)" >> "$LOG_FILE"
echo "" >> "$LOG_FILE"

log "SecuraCV Canary — Key Generation (Stage 0)"
log "==========================================="
echo ""

# Check for existing keys
if [[ -f "$KEY_DIR/secure_boot_signing_key_0.pem" ]]; then
    err "Signing key already exists at $KEY_DIR/secure_boot_signing_key_0.pem"
    err "If you need to regenerate, manually remove the keys/ directory first."
    err "This is a safety check to prevent accidental key replacement."
    exit 1
fi

# Check tools
for tool in espsecure.py openssl; do
    if ! command -v "$tool" &>/dev/null; then
        # Try ESP-IDF path
        if [[ -n "${IDF_PATH:-}" ]] && [[ -f "$IDF_PATH/tools/espsecure.py" ]]; then
            log "Found espsecure.py in IDF_PATH"
        else
            err "Required tool not found: $tool"
            err "Ensure ESP-IDF is installed and sourced (. \$IDF_PATH/export.sh)"
            exit 1
        fi
    fi
done

# Check entropy
ENTROPY=$(cat /proc/sys/kernel/random/entropy_avail 2>/dev/null || echo "unknown")
log "Available entropy: $ENTROPY"
if [[ "$ENTROPY" != "unknown" ]] && [[ "$ENTROPY" -lt 256 ]]; then
    warn "Low entropy ($ENTROPY). Consider using rng-tools or haveged."
    warn "Key generation will proceed but quality may be reduced."
fi

# ── Confirmation ────────────────────────────────────────────────────────────

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  SecuraCV Canary — Cryptographic Key Generation             ║"
echo "║                                                              ║"
echo "║  This will generate:                                         ║"
echo "║    • RSA-3072 Secure Boot v2 signing key (primary)           ║"
echo "║    • RSA-3072 Secure Boot v2 signing key (rotation slot)     ║"
echo "║    • 256-bit XTS-AES flash encryption key                    ║"
echo "║                                                              ║"
echo "║  THESE KEYS ARE IRREPLACEABLE FOR PROVISIONED DEVICES.       ║"
echo "║  LOSS = BRICKED DEVICES. LEAK = COMPROMISED BOOT CHAIN.     ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
read -p "Generate keys? Type 'GENERATE' to confirm: " CONFIRM
if [[ "$CONFIRM" != "GENERATE" ]]; then
    log "Aborted by user."
    exit 0
fi

# ── Generate Secure Boot Signing Keys ───────────────────────────────────────

log ""
log "Generating Secure Boot v2 signing key (primary — slot 0)..."
espsecure.py generate_signing_key \
    --version 2 \
    --scheme rsa3072 \
    "$KEY_DIR/secure_boot_signing_key_0.pem" \
    2>&1 | tee -a "$LOG_FILE"
ok "Primary signing key generated."

log "Generating Secure Boot v2 signing key (rotation — slot 1)..."
espsecure.py generate_signing_key \
    --version 2 \
    --scheme rsa3072 \
    "$KEY_DIR/secure_boot_signing_key_1.pem" \
    2>&1 | tee -a "$LOG_FILE"
ok "Rotation signing key generated."

# Extract public key digests for reference
log "Extracting public key digests..."
for i in 0 1; do
    openssl rsa -in "$KEY_DIR/secure_boot_signing_key_${i}.pem" \
        -pubout -out "$KEY_DIR/secure_boot_public_key_${i}.pem" 2>/dev/null
    
    # Generate the digest that will be burned to eFuse
    espsecure.py digest_sbv2_public_key \
        --keyfile "$KEY_DIR/secure_boot_signing_key_${i}.pem" \
        --output "$KEY_DIR/secure_boot_digest_${i}.bin" \
        2>&1 | tee -a "$LOG_FILE"
    
    DIGEST_HEX=$(xxd -p "$KEY_DIR/secure_boot_digest_${i}.bin" | tr -d '\n')
    log "  Slot $i digest: ${DIGEST_HEX:0:32}..."
done
ok "Public key digests extracted."

# ── Generate Flash Encryption Key ───────────────────────────────────────────

log ""
log "Generating 256-bit XTS-AES flash encryption key..."

# For ESP32-S3, XTS_AES_128_KEY uses a 256-bit key (confusing naming)
# XTS_AES_256_KEY uses a 512-bit key split across two key blocks
# We use XTS_AES_128_KEY (256-bit) for simplicity — still AES-256-XTS
espsecure.py generate_flash_encryption_key \
    --keylen 256 \
    "$KEY_DIR/flash_encryption_key.bin" \
    2>&1 | tee -a "$LOG_FILE"
ok "Flash encryption key generated."

FE_KEY_HEX=$(xxd -p "$KEY_DIR/flash_encryption_key.bin" | tr -d '\n')
log "  Key fingerprint: ${FE_KEY_HEX:0:16}...${FE_KEY_HEX: -16}"

# ── Create .gitignore ──────────────────────────────────────────────────────

cat > "$KEY_DIR/.gitignore" << 'EOF'
# NEVER commit cryptographic keys to version control
*.pem
*.bin
*.key
!.gitignore
EOF

# ── Generate key inventory file ────────────────────────────────────────────

cat > "$KEY_DIR/KEY_INVENTORY.txt" << EOF
# SecuraCV Canary — Key Inventory
# Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)
# Host: $(hostname)
# Operator: $(whoami)
#
# WARNING: This file contains metadata ONLY, not key material.
# Actual keys are in the sibling .pem and .bin files.
#
# ── Secure Boot Signing Keys ──
# Scheme: RSA-3072 (Secure Boot v2)
# Slot 0 (primary):  secure_boot_signing_key_0.pem
#   Digest SHA256:    $(sha256sum "$KEY_DIR/secure_boot_signing_key_0.pem" | cut -d' ' -f1)
# Slot 1 (rotation): secure_boot_signing_key_1.pem
#   Digest SHA256:    $(sha256sum "$KEY_DIR/secure_boot_signing_key_1.pem" | cut -d' ' -f1)
#
# ── Flash Encryption Key ──
# Algorithm: XTS-AES-128 (256-bit key)
# File:      flash_encryption_key.bin
#   SHA256:   $(sha256sum "$KEY_DIR/flash_encryption_key.bin" | cut -d' ' -f1)
#
# ── Backup Checklist ──
# [ ] Keys copied to encrypted offline storage (USB drive, safe)
# [ ] Keys copied to second encrypted offline backup
# [ ] This host's disk is encrypted (LUKS, FileVault, BitLocker)
# [ ] Key access is logged and audited
EOF

ok "Key inventory written to $KEY_DIR/KEY_INVENTORY.txt"

# ── Set permissions ─────────────────────────────────────────────────────────

chmod 600 "$KEY_DIR"/*.pem "$KEY_DIR"/*.bin
chmod 644 "$KEY_DIR/KEY_INVENTORY.txt" "$KEY_DIR/.gitignore"
chmod 700 "$KEY_DIR"

ok "File permissions secured (700 dir, 600 keys)."

# ── Summary ─────────────────────────────────────────────────────────────────

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  KEY GENERATION COMPLETE                                     ║"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║                                                              ║"
echo "║  Files created in: keys/                                     ║"
echo "║    secure_boot_signing_key_0.pem  (primary signing key)      ║"
echo "║    secure_boot_signing_key_1.pem  (rotation signing key)     ║"
echo "║    secure_boot_public_key_0.pem   (public key, slot 0)       ║"
echo "║    secure_boot_public_key_1.pem   (public key, slot 1)       ║"
echo "║    secure_boot_digest_0.bin       (eFuse digest, slot 0)     ║"
echo "║    secure_boot_digest_1.bin       (eFuse digest, slot 1)     ║"
echo "║    flash_encryption_key.bin       (per-device or shared)     ║"
echo "║    KEY_INVENTORY.txt              (metadata / checklist)     ║"
echo "║                                                              ║"
echo "║  ⚠  BACK UP THESE KEYS TO ENCRYPTED OFFLINE STORAGE NOW ⚠  ║"
echo "║  ⚠  LOSS OF KEYS = INABILITY TO UPDATE DEVICES           ⚠  ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
log "Key generation complete. Log: $LOG_FILE"
