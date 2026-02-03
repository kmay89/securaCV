# SecuraCV Canary WAP - Build Makefile
#
# Unified build system for PlatformIO and Arduino IDE.
#
# Quick Start:
#   make setup      - Interactive setup wizard
#   make build      - Build with PlatformIO (default)
#   make upload     - Build and upload to device
#   make monitor    - Monitor serial output
#
# For Arduino IDE users:
#   make setup-arduino - Prepare Arduino sketch folder
#   make arduino-build - Build with arduino-cli (if installed)
#

.PHONY: all build upload monitor clean help
.PHONY: build-mobile build-debug build-all
.PHONY: setup setup-platformio setup-arduino
.PHONY: arduino-build arduino-upload
.PHONY: test lint format
.PHONY: secrets check-deps info

# Configuration
BOARD ?= seeed_xiao_esp32s3
PORT ?= auto
ARDUINO_FQBN ?= esp32:esp32:esp32s3
ARDUINO_DIR ?= arduino/canary_wap

# Default target
all: build

# ============================================================================
# SETUP
# ============================================================================

setup:
	@./setup.sh

setup-platformio:
	@./setup.sh platformio

setup-arduino:
	@./setup.sh arduino

check-deps:
	@./setup.sh check

# ============================================================================
# PLATFORMIO BUILD (RECOMMENDED)
# ============================================================================

build:
	pio run

build-mobile:
	pio run -e canary-wap-mobile

build-debug:
	pio run -e canary-wap-debug

build-all:
	pio run -e canary-wap-default
	pio run -e canary-wap-mobile

# Upload targets
upload:
	pio run -t upload

upload-mobile:
	pio run -e canary-wap-mobile -t upload

upload-debug:
	pio run -e canary-wap-debug -t upload

# Monitor serial output
monitor:
	pio device monitor -b 115200

# Build and monitor in one command
run: upload monitor

# ============================================================================
# ARDUINO CLI BUILD (ALTERNATIVE)
# ============================================================================

# Check if arduino-cli is available
ARDUINO_CLI := $(shell command -v arduino-cli 2>/dev/null)

arduino-build:
ifdef ARDUINO_CLI
	@echo "Building with Arduino CLI..."
	arduino-cli compile --fqbn $(ARDUINO_FQBN) \
		--build-property "build.extra_flags=-DARDUINO_USB_CDC_ON_BOOT=1" \
		$(ARDUINO_DIR)
else
	@echo "arduino-cli not found. Install from: https://arduino.github.io/arduino-cli/"
	@echo "Or use PlatformIO: make build"
	@exit 1
endif

arduino-upload:
ifdef ARDUINO_CLI
	@echo "Uploading with Arduino CLI..."
	arduino-cli upload --fqbn $(ARDUINO_FQBN) \
		$(if $(filter-out auto,$(PORT)),-p $(PORT),) \
		$(ARDUINO_DIR)
else
	@echo "arduino-cli not found"
	@exit 1
endif

arduino-monitor:
ifdef ARDUINO_CLI
	arduino-cli monitor -p $(PORT) -c baudrate=115200
else
	@echo "arduino-cli not found"
	@exit 1
endif

# ============================================================================
# SECRETS MANAGEMENT
# ============================================================================

secrets:
	@if [ ! -f secrets/secrets.h ]; then \
		if [ -f secrets/secrets.example.h ]; then \
			cp secrets/secrets.example.h secrets/secrets.h; \
			echo "Created secrets/secrets.h from template"; \
			echo "IMPORTANT: Edit secrets/secrets.h with your credentials"; \
			echo "           Change the default AP password before deployment!"; \
		else \
			echo "Error: secrets/secrets.example.h not found"; \
			exit 1; \
		fi \
	else \
		echo "secrets/secrets.h already exists"; \
	fi

# ============================================================================
# CLEAN
# ============================================================================

clean:
	pio run -t clean

distclean:
	rm -rf .pio
	rm -rf $(ARDUINO_DIR)/build

# ============================================================================
# CODE QUALITY
# ============================================================================

lint:
	@echo "Running static analysis..."
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=warning,style --std=c++17 \
			-I ../../common -I ../../boards/xiao-esp32s3-sense/pins \
			-I ../../configs/canary-wap/default \
			src/ include/ 2>&1 | head -50; \
	else \
		echo "cppcheck not found, skipping"; \
	fi

format:
	@echo "Formatting code..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find src include -name "*.cpp" -o -name "*.h" 2>/dev/null | xargs -r clang-format -i; \
		echo "Done"; \
	else \
		echo "clang-format not found, skipping"; \
	fi

# ============================================================================
# INFO & HELP
# ============================================================================

info:
	@echo ""
	@echo "SecuraCV Canary WAP"
	@echo "==================="
	@echo ""
	@echo "Board:   XIAO ESP32-S3 Sense"
	@echo "MCU:     ESP32-S3 (240 MHz, 8MB Flash, 8MB PSRAM)"
	@echo "Camera:  OV2640 (2MP)"
	@echo ""
	@echo "Features:"
	@echo "  - Ed25519 signed witness records"
	@echo "  - GPS/GNSS location tracking"
	@echo "  - SD card append-only storage"
	@echo "  - WiFi AP for local monitoring"
	@echo "  - HTTP REST API & Web UI"
	@echo "  - Opera mesh networking"
	@echo "  - Bluetooth pairing"
	@echo "  - RF presence detection"
	@echo ""
	@echo "Build configs:"
	@echo "  - default: Full-featured"
	@echo "  - mobile:  Power-optimized"
	@echo "  - debug:   Verbose logging"
	@echo ""

help:
	@echo ""
	@echo "SecuraCV Canary WAP - Build Targets"
	@echo "===================================="
	@echo ""
	@echo "Setup:"
	@echo "  make setup           - Interactive setup wizard"
	@echo "  make setup-platformio - Setup for PlatformIO"
	@echo "  make setup-arduino    - Setup for Arduino IDE"
	@echo "  make check-deps      - Check dependencies"
	@echo "  make secrets         - Create secrets.h template"
	@echo ""
	@echo "PlatformIO Build (recommended):"
	@echo "  make build           - Build default configuration"
	@echo "  make build-mobile    - Build power-optimized"
	@echo "  make build-debug     - Build with verbose logging"
	@echo "  make build-all       - Build all configurations"
	@echo "  make upload          - Build and upload to device"
	@echo "  make monitor         - Monitor serial output (115200)"
	@echo "  make run             - Upload and monitor"
	@echo ""
	@echo "Arduino CLI Build (alternative):"
	@echo "  make arduino-build   - Compile with arduino-cli"
	@echo "  make arduino-upload  - Upload with arduino-cli"
	@echo "  make arduino-monitor - Monitor with arduino-cli"
	@echo ""
	@echo "Maintenance:"
	@echo "  make clean           - Clean build artifacts"
	@echo "  make distclean       - Full clean"
	@echo "  make lint            - Run static analysis"
	@echo "  make format          - Format code"
	@echo ""
	@echo "Info:"
	@echo "  make info            - Show project information"
	@echo "  make help            - Show this help"
	@echo ""
