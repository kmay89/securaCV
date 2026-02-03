# SecuraCV Canary Vision - Build Makefile
#
# Vision AI presence detection with MQTT publishing.
#
# Quick Start:
#   make build   - Build firmware
#   make upload  - Build and upload to device
#   make monitor - Monitor serial output
#

.PHONY: all build upload monitor clean help
.PHONY: build-debug secrets info

BAUD ?= 115200

# Default target
all: build

# ============================================================================
# BUILD
# ============================================================================

build:
	pio run

build-debug:
	pio run -e canary-vision-debug

# Upload targets
upload:
	pio run -t upload

upload-debug:
	pio run -e canary-vision-debug -t upload

# Monitor serial output
monitor:
	pio device monitor -b $(BAUD)

# Build and monitor
run: upload monitor

# ============================================================================
# SECRETS
# ============================================================================

secrets:
	@if [ ! -f secrets/secrets.h ]; then \
		if [ -f secrets/secrets.example.h ]; then \
			cp secrets/secrets.example.h secrets/secrets.h; \
			echo "Created secrets/secrets.h from template"; \
			echo "IMPORTANT: Edit secrets/secrets.h with your credentials"; \
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

# ============================================================================
# INFO & HELP
# ============================================================================

info:
	@echo ""
	@echo "SecuraCV Canary Vision"
	@echo "======================"
	@echo ""
	@echo "Board:   ESP32-C3 DevKit"
	@echo "Sensor:  Grove Vision AI V2 (SSCMA)"
	@echo ""
	@echo "Features:"
	@echo "  - Person detection with confidence"
	@echo "  - Presence/dwelling state machine"
	@echo "  - MQTT publishing to Home Assistant"
	@echo "  - Auto-discovery for HA integration"
	@echo ""
	@echo "MQTT Topics:"
	@echo "  - securacv/<device_id>/events"
	@echo "  - securacv/<device_id>/state"
	@echo "  - securacv/<device_id>/status"
	@echo ""

help:
	@echo ""
	@echo "SecuraCV Canary Vision - Build Targets"
	@echo "======================================="
	@echo ""
	@echo "Build:"
	@echo "  make build       - Build default configuration"
	@echo "  make build-debug - Build with verbose logging"
	@echo "  make upload      - Build and upload to device"
	@echo "  make monitor     - Monitor serial output"
	@echo "  make run         - Upload and monitor"
	@echo ""
	@echo "Setup:"
	@echo "  make secrets     - Create secrets.h template"
	@echo ""
	@echo "Maintenance:"
	@echo "  make clean       - Clean build artifacts"
	@echo "  make distclean   - Full clean"
	@echo ""
	@echo "Info:"
	@echo "  make info        - Show project information"
	@echo "  make help        - Show this help"
	@echo ""
