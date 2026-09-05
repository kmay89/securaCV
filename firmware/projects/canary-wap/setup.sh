#!/bin/bash
set -euo pipefail
#
# SecuraCV Canary WAP - Setup Script
#
# This script prepares the build environment for either:
#   - PlatformIO (recommended)
#   - Arduino IDE
#
# Usage:
#   ./setup.sh              # Interactive mode
#   ./setup.sh platformio   # Setup for PlatformIO
#   ./setup.sh arduino      # Setup for Arduino IDE
#   ./setup.sh check        # Check dependencies
#


# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

print_header() {
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║${NC}  SecuraCV Canary WAP - Build Setup                            ${BLUE}║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}!${NC} $1"
}

print_info() {
    echo -e "${BLUE}→${NC} $1"
}

check_command() {
    if command -v "$1" &> /dev/null; then
        print_success "$1 found: $(command -v $1)"
        return 0
    else
        print_error "$1 not found"
        return 1
    fi
}

check_dependencies() {
    echo ""
    echo "Checking dependencies..."
    echo ""

    local all_ok=true

    # Check for git
    if ! check_command "git"; then
        all_ok=false
    fi

    # Check for Python
    if ! check_command "python3"; then
        all_ok=false
    fi

    # Check for PlatformIO
    if check_command "pio"; then
        PIO_VERSION=$(pio --version 2>/dev/null | head -1)
        print_info "PlatformIO: $PIO_VERSION"
    else
        print_warn "PlatformIO not installed (needed for PlatformIO builds)"
        print_info "Install: pip install platformio"
    fi

    # Check for Arduino CLI (optional)
    if check_command "arduino-cli"; then
        ARDUINO_VERSION=$(arduino-cli version 2>/dev/null | head -1)
        print_info "Arduino CLI: $ARDUINO_VERSION"
    else
        print_warn "Arduino CLI not installed (optional, for command-line Arduino builds)"
        print_info "Install: https://arduino.github.io/arduino-cli/latest/installation/"
    fi

    echo ""

    if [ "$all_ok" = true ]; then
        print_success "All required dependencies are installed"
    else
        print_error "Some dependencies are missing"
    fi

    return 0
}

setup_secrets() {
    local secrets_dir="${SCRIPT_DIR}/secrets"
    local secrets_file="${secrets_dir}/secrets.h"
    local example_file="${secrets_dir}/secrets.example.h"

    if [ ! -d "$secrets_dir" ]; then
        mkdir -p "$secrets_dir"
    fi

    if [ ! -f "$example_file" ]; then
        cat > "$example_file" << 'EOF'
/**
 * @file secrets.h
 * @brief canary-wap takes NO compile-time credentials.
 *
 * This file exists because setup.sh has always created it and the Arduino
 * IDE workflow expects it to be present; nothing in the firmware includes it.
 * It used to advertise Wi-Fi, MQTT, AP-password and API-token macros that no
 * source ever read — including an AP_PASSWORD_CUSTOM "change this before
 * deployment" instruction that changed nothing.
 *
 * Where each credential actually lives:
 *   - Wi-Fi station credentials: provisioned over the captive portal at first
 *     boot and stored in NVS (canary_wap.ino, NVS_KEY_WIFI_SSID / _PASS).
 *   - The setup-AP password: derived per device from the private key
 *     (derive_ap_password) and shown on the serial banner and /enroll page.
 *   - The API token: derived per device from the private key
 *     (derive_api_token), stored in NVS, shown on the serial banner.
 *   - MQTT: configured at runtime through the device web UI.
 *
 * Leave this file empty. Adding macros here does nothing.
 */

#pragma once
EOF
        print_success "Created secrets/secrets.example.h"
    fi

    if [ ! -f "$secrets_file" ]; then
        cp "$example_file" "$secrets_file"
        print_success "Created secrets/secrets.h (nothing to edit — canary-wap takes no compile-time credentials)"
    else
        print_info "secrets/secrets.h already exists"
    fi
}

setup_platformio() {
    echo ""
    echo "Setting up for PlatformIO..."
    echo ""

    # Create secrets
    setup_secrets

    # Verify PlatformIO
    if ! command -v pio &> /dev/null; then
        print_error "PlatformIO not installed"
        print_info "Install with: pip install platformio"
        return 1
    fi

    # Install ESP32 platform
    print_info "Installing ESP32 platform..."
    pio pkg install --global --platform espressif32 2>/dev/null || true

    # Install required libraries
    print_info "Installing libraries..."
    pio pkg install --global --library "bblanchon/ArduinoJson@^7.0.0" 2>/dev/null || true
    pio pkg install --global --library "rweather/Crypto@^0.4.0" 2>/dev/null || true
    pio pkg install --global --library "h2zero/NimBLE-Arduino@^2.3.8" 2>/dev/null || true

    print_success "PlatformIO setup complete!"
    echo ""
    echo "Build commands:"
    echo "  cd ${SCRIPT_DIR}"
    echo "  pio run                     # Build default configuration"
    echo "  pio run -e canary-wap-mobile  # Build power-optimized"
    echo "  pio run -t upload           # Build and upload"
    echo "  pio device monitor          # Monitor serial output"
    echo ""
    echo "Or use make:"
    echo "  make build"
    echo "  make upload"
    echo "  make monitor"
    echo ""
}

setup_arduino() {
    echo ""
    echo "Setting up for Arduino IDE..."
    echo ""

    local arduino_dir="${SCRIPT_DIR}/arduino/canary_wap"

    # Create secrets in Arduino folder
    if [ ! -f "${arduino_dir}/secrets.h" ]; then
        if [ -f "${SCRIPT_DIR}/secrets/secrets.h" ]; then
            cp "${SCRIPT_DIR}/secrets/secrets.h" "${arduino_dir}/secrets.h"
        else
            setup_secrets
            cp "${SCRIPT_DIR}/secrets/secrets.h" "${arduino_dir}/secrets.h"
        fi
        print_success "Created Arduino secrets.h"
    fi

    # Copy board pins to Arduino folder
    local pins_src="${FIRMWARE_ROOT}/boards/xiao-esp32s3-sense/pins"
    if [ -d "$pins_src" ]; then
        cp "${pins_src}/pins.h" "${arduino_dir}/pins.h" 2>/dev/null || true
        cp "${pins_src}/camera.h" "${arduino_dir}/camera.h" 2>/dev/null || true
        print_success "Copied board pin definitions"
    fi

    # Copy config to Arduino folder
    local config_src="${FIRMWARE_ROOT}/configs/canary-wap/default"
    if [ -d "$config_src" ]; then
        cp "${config_src}/config.h" "${arduino_dir}/config.h" 2>/dev/null || true
        print_success "Copied configuration"
    fi

    # Stage the SecuraCV CSI library next to the sketch. Arduino IDE only
    # searches the sketch directory, the sketch's `src/` subtree, and globally
    # installed libraries — so the library that lives in firmware/common/csi/
    # is otherwise invisible. csi_integration.h does `#include <csi_types.h>`
    # and several .cpp files in the sketch (csi_integration.cpp, rf_presence.cpp,
    # tests.cpp) include other headers from the same library.
    #
    # The library is currently flat (no subdirectories under src/), so a shallow
    # copy of the top-level *.h / *.cpp is sufficient. If subdirectories ever
    # get introduced under firmware/common/csi/src/, this needs to switch to
    # `find` — the Arduino sketch directory must stay flat for `<csi_types.h>`
    # to resolve, so nested files would have to be flattened or re-homed.
    local csi_src="${FIRMWARE_ROOT}/common/csi/src"
    if [ -d "$csi_src" ]; then
        cp "${csi_src}"/*.h "${arduino_dir}/" 2>/dev/null || true
        cp "${csi_src}"/*.cpp "${arduino_dir}/" 2>/dev/null || true
        if [ -f "${arduino_dir}/csi_types.h" ]; then
            print_success "Copied CSI library headers and sources"
        else
            print_error "Failed to stage CSI library files to ${arduino_dir}"
            return 1
        fi
    else
        print_warn "CSI library not found at ${csi_src} — sketch will not compile until firmware/common/csi/src/ is restored"
    fi

    # Stage the shared boot banner library next to the sketch. The sketch does
    # `#include <boot_banner.h>`; like the CSI library it must be flattened into
    # the sketch directory because Arduino IDE only searches the sketch dir and
    # globally installed libraries.
    local boot_src="${FIRMWARE_ROOT}/common/boot"
    if [ -d "$boot_src" ]; then
        cp "${boot_src}/boot_banner.h" "${arduino_dir}/" 2>/dev/null || true
        cp "${boot_src}/boot_banner.cpp" "${arduino_dir}/" 2>/dev/null || true
        if [ -f "${arduino_dir}/boot_banner.h" ]; then
            print_success "Copied boot banner library"
        else
            print_error "Failed to stage boot banner files to ${arduino_dir}"
            return 1
        fi
    else
        print_warn "Boot banner not found at ${boot_src} — sketch will not compile until firmware/common/boot/ is restored"
    fi

    # The salted device pseudonym (operator-facing diagnostic handle) is a single
    # canonical, header-only source shared with the canary-vision tree. Stage a
    # byte-identical copy next to the sketch so Arduino's flat include search finds
    # it; check_csi_sync.sh guards the two against drift.
    local identity_src="${FIRMWARE_ROOT}/common/identity"
    if [ -f "${identity_src}/device_pseudonym.h" ]; then
        cp "${identity_src}/device_pseudonym.h" "${arduino_dir}/" 2>/dev/null || true
        if [ -f "${arduino_dir}/device_pseudonym.h" ]; then
            print_success "Copied device_pseudonym header"
        else
            print_error "Failed to stage device_pseudonym header to ${arduino_dir}"
            return 1
        fi
    else
        print_warn "device_pseudonym not found at ${identity_src} — sketch will not compile until firmware/common/identity/ is restored"
    fi

    # The witness-store pure logic (the /WITNESS/records.jsonl line format +
    # SD-wins boot reconciliation) is a single canonical, header-only source
    # shared with the PIO canary tree (firmware/canary). Stage a byte-identical
    # copy next to the sketch; check_csi_sync.sh guards the two against drift.
    local witness_src="${FIRMWARE_ROOT}/common/witness"
    if [ -f "${witness_src}/witness_store.h" ]; then
        cp "${witness_src}/witness_store.h" "${arduino_dir}/" 2>/dev/null || true
        if [ -f "${arduino_dir}/witness_store.h" ]; then
            print_success "Copied witness_store header"
        else
            print_error "Failed to stage witness_store header to ${arduino_dir}"
            return 1
        fi
    else
        print_warn "witness_store not found at ${witness_src} — sketch will not compile until firmware/common/witness/ is restored"
    fi

    # GNSS UTC date/time -> validated Unix epoch (the NMEA-time trust window +
    # calendar math the GPS-derived system clock relies on) is a single
    # canonical, header-only source shared with the PIO canary tree
    # (firmware/canary). Stage a byte-identical copy next to the sketch;
    # check_csi_sync.sh guards the two against drift.
    local gnss_src="${FIRMWARE_ROOT}/common/gnss"
    if [ -f "${gnss_src}/gnss_time.h" ]; then
        cp "${gnss_src}/gnss_time.h" "${arduino_dir}/" 2>/dev/null || true
        if [ -f "${arduino_dir}/gnss_time.h" ]; then
            print_success "Copied gnss_time header"
        else
            print_error "Failed to stage gnss_time header to ${arduino_dir}"
            return 1
        fi
    else
        print_warn "gnss_time not found at ${gnss_src} — sketch will not compile until firmware/common/gnss/ is restored"
    fi

    print_success "Arduino IDE setup complete!"
    echo ""
    echo "Arduino IDE Instructions:"
    echo ""
    echo "1. Install Board Support:"
    echo "   - Open Arduino IDE"
    echo "   - Go to File → Preferences"
    echo "   - Add to 'Additional Boards Manager URLs':"
    echo "     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
    echo "   - Go to Tools → Board → Boards Manager"
    echo "   - Search 'esp32' and install 'esp32 by Espressif Systems'"
    echo ""
    echo "2. Install Libraries (Tools → Manage Libraries):"
    echo "   - ArduinoJson by Benoit Blanchon (version 7.x)"
    echo "   - Crypto by Rhys Weatherley"
    echo "   - NimBLE-Arduino by h2zero (version 2.3.8 or later — DO NOT use 1.x)"
    echo ""
    echo "3. Board Settings (Tools menu):"
    echo "   - Board: XIAO_ESP32S3 (or ESP32S3 Dev Module)"
    echo "   - USB CDC On Boot: Enabled"
    echo "   - Flash Size: 8MB"
    echo "   - PSRAM: OPI PSRAM   <-- DEFAULTS TO 'Disabled'. The camera falls"
    echo "                            back to QVGA-in-DRAM unless you set this!"
    echo ""
    echo "   Tip: Arduino IDE 2.3+ can apply these for you — open the sketch and"
    echo "        pick the 'xiao_sense' profile (from sketch.yaml) in the toolbar"
    echo "        dropdown instead of setting each Tools-menu option by hand."
    echo ""
    echo "4. Open the sketch:"
    echo "   ${arduino_dir}/canary_wap.ino"
    echo ""
    echo "5. Click Upload!"
    echo ""

    # If arduino-cli is available, offer to install boards/libraries
    if command -v arduino-cli &> /dev/null; then
        echo ""
        read -p "Install boards and libraries with arduino-cli? [y/N] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            print_info "Installing ESP32 board..."
            arduino-cli config init --overwrite 2>/dev/null || true
            arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json 2>/dev/null || true
            arduino-cli core update-index
            arduino-cli core install esp32:esp32

            print_info "Installing libraries..."
            arduino-cli lib install "ArduinoJson"
            arduino-cli lib install "Crypto"
            arduino-cli lib install "NimBLE-Arduino"

            print_success "Arduino CLI setup complete!"
            echo ""
            print_info "Build/upload with PSRAM baked in (uses the sketch.yaml profile):"
            echo "    arduino-cli compile --profile xiao_sense ${arduino_dir}"
            echo "    arduino-cli upload  --profile xiao_sense -p <PORT> ${arduino_dir}"
            print_info "Or via make (same PSRAM-enabled FQBN):  make arduino-build / make arduino-upload"
        fi
    fi
}

show_menu() {
    print_header

    echo "Select your build environment:"
    echo ""
    echo "  1) PlatformIO (Recommended)"
    echo "     - Full feature support"
    echo "     - Multiple build configurations"
    echo "     - Integrated library management"
    echo ""
    echo "  2) Arduino IDE"
    echo "     - Simple setup"
    echo "     - Familiar interface"
    echo "     - Good for quick modifications"
    echo ""
    echo "  3) Check dependencies"
    echo ""
    echo "  4) Exit"
    echo ""

    read -p "Enter choice [1-4]: " choice

    case $choice in
        1)
            setup_platformio
            ;;
        2)
            setup_arduino
            ;;
        3)
            check_dependencies
            ;;
        4)
            echo "Goodbye!"
            exit 0
            ;;
        *)
            print_error "Invalid choice"
            exit 1
            ;;
    esac
}

# Main
case "${1:-}" in
    platformio|pio)
        setup_platformio
        ;;
    arduino)
        setup_arduino
        ;;
    check)
        check_dependencies
        ;;
    help|--help|-h)
        echo "Usage: $0 [platformio|arduino|check|help]"
        echo ""
        echo "Commands:"
        echo "  platformio  Setup for PlatformIO builds"
        echo "  arduino     Setup for Arduino IDE builds"
        echo "  check       Check dependencies"
        echo "  help        Show this help"
        echo ""
        echo "Run without arguments for interactive mode."
        ;;
    "")
        show_menu
        ;;
    *)
        print_error "Unknown command: $1"
        echo "Run '$0 help' for usage"
        exit 1
        ;;
esac
