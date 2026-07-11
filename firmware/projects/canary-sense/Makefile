# SecuraCV Canary Sense - Build Makefile
#
# 60GHz mmWave radar presence (and optional wellbeing vitals) witness
# on the Seeed MR60BHA2 kit (XIAO ESP32-C6 host).
#
# Quick Start:
#   make build   - Build firmware
#   make upload  - Build and upload to device
#   make monitor - Monitor serial output
#

.PHONY: all build upload monitor clean help
.PHONY: build-wellbeing build-debug upload-wellbeing upload-debug
.PHONY: secrets info distclean run

BAUD ?= 115200

# Default target
all: build

# ============================================================================
# BUILD
# ============================================================================

build:
	pio run

build-wellbeing:
	pio run -e canary-sense-wellbeing

build-debug:
	pio run -e canary-sense-debug

# Upload targets
upload:
	pio run -t upload

upload-wellbeing:
	pio run -e canary-sense-wellbeing -t upload

upload-debug:
	pio run -e canary-sense-debug -t upload

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
	@echo "SecuraCV Canary Sense"
	@echo "====================="
	@echo ""
	@echo "Board:   XIAO ESP32-C6 (Seeed MR60BHA2 kit)"
	@echo "Sensor:  60GHz mmWave radar (MR60BHA2) + BH1750 lux"
	@echo ""
	@echo "Features:"
	@echo "  - Radar presence with 0/1/2+ occupant bucket"
	@echo "  - Optional P1-gated wellbeing vitals (wellbeing build)"
	@echo "  - Ed25519 signed witness chain"
	@echo "  - MQTT publishing to Home Assistant"
	@echo "  - Auto-discovery for HA integration"
	@echo "  - Signed pull-OTA with HA update entity"
	@echo ""
	@echo "MQTT Topics:"
	@echo "  - securacv/<device_id>/events"
	@echo "  - securacv/<device_id>/state"
	@echo "  - securacv/<device_id>/status"
	@echo "  - securacv/<device_id>/chain"
	@echo "  - securacv/<device_id>/health"
	@echo ""

help:
	@echo ""
	@echo "SecuraCV Canary Sense - Build Targets"
	@echo "======================================"
	@echo ""
	@echo "Build:"
	@echo "  make build            - Build default configuration (presence-only)"
	@echo "  make build-wellbeing  - Build with P1-gated vitals (-DCANARY_SENSE_VITALS)"
	@echo "  make build-debug      - Build with verbose logging"
	@echo "  make upload           - Build and upload to device"
	@echo "  make upload-wellbeing - Upload the wellbeing build"
	@echo "  make monitor          - Monitor serial output"
	@echo "  make run              - Upload and monitor"
	@echo ""
	@echo "Setup:"
	@echo "  make secrets          - Create secrets.h template"
	@echo ""
	@echo "Maintenance:"
	@echo "  make clean            - Clean build artifacts"
	@echo "  make distclean        - Full clean"
	@echo ""
	@echo "Info:"
	@echo "  make info             - Show project information"
	@echo "  make help             - Show this help"
	@echo ""
