# SecuraCV Firmware - Root Makefile
#
# Quick commands for building firmware with different toolchains.
#
# Usage:
#   make arduino APP=canary-wap CONFIG=default
#   make pio-build APP=canary-wap CONFIG=default
#   make help
#

.PHONY: help arduino pio-build pio-upload pio-monitor clean list-configs

# Default values
APP ?= canary-wap
CONFIG ?= default

# Directories
FIRMWARE_DIR := $(shell pwd)
SCRIPTS_DIR := $(FIRMWARE_DIR)/scripts
BUILD_DIR := $(FIRMWARE_DIR)/build

# Computed values
SKETCH_NAME := $(subst -,_,$(APP))_$(CONFIG)

help:
	@echo "SecuraCV Firmware Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make <target> APP=<app-id> CONFIG=<config-id>"
	@echo ""
	@echo "Targets:"
	@echo "  arduino      - Build Arduino IDE sketch"
	@echo "  pio-build    - Build with PlatformIO"
	@echo "  pio-upload   - Build and upload with PlatformIO"
	@echo "  pio-monitor  - Open serial monitor"
	@echo "  clean        - Remove build artifacts"
	@echo "  list-configs - Show available configurations"
	@echo ""
	@echo "Examples:"
	@echo "  make arduino APP=canary-wap CONFIG=default"
	@echo "  make arduino APP=canary-wap CONFIG=mobile"
	@echo "  make arduino APP=canary-vision CONFIG=default"
	@echo ""
	@echo "  make pio-build APP=canary-wap CONFIG=default"
	@echo "  make pio-upload APP=canary-wap CONFIG=default"
	@echo ""

list-configs:
	@echo "Available configurations:"
	@echo ""
	@echo "Canary WAP (ESP32-S3):"
	@echo "  APP=canary-wap CONFIG=default  - Full-featured WAP"
	@echo "  APP=canary-wap CONFIG=mobile   - Power-optimized"
	@echo ""
	@echo "Canary Vision (ESP32-C3):"
	@echo "  APP=canary-vision CONFIG=default - Vision AI"
	@echo ""

# ============================================================================
# Arduino IDE
# ============================================================================

arduino:
	@echo "Building Arduino sketch for $(APP) / $(CONFIG)..."
	@$(SCRIPTS_DIR)/build_arduino.sh $(APP) $(CONFIG)
	@echo ""
	@echo "Sketch ready at: $(BUILD_DIR)/arduino/$(SKETCH_NAME)/"
	@echo "Open in Arduino IDE: $(BUILD_DIR)/arduino/$(SKETCH_NAME)/$(SKETCH_NAME).ino"

arduino-open: arduino
	@echo "Opening in Arduino IDE..."
	@if command -v arduino-cli &> /dev/null; then \
		arduino-cli sketch open "$(BUILD_DIR)/arduino/$(SKETCH_NAME)"; \
	elif command -v open &> /dev/null; then \
		open "$(BUILD_DIR)/arduino/$(SKETCH_NAME)/$(SKETCH_NAME).ino"; \
	elif command -v xdg-open &> /dev/null; then \
		xdg-open "$(BUILD_DIR)/arduino/$(SKETCH_NAME)/$(SKETCH_NAME).ino"; \
	else \
		echo "Please open manually: $(BUILD_DIR)/arduino/$(SKETCH_NAME)/$(SKETCH_NAME).ino"; \
	fi

# ============================================================================
# PlatformIO
# ============================================================================

pio-build:
	@echo "Building with PlatformIO: $(APP)-$(CONFIG)..."
	@cd $(FIRMWARE_DIR)/projects/$(APP) && pio run -e $(APP)-$(CONFIG)

pio-upload:
	@echo "Uploading with PlatformIO: $(APP)-$(CONFIG)..."
	@cd $(FIRMWARE_DIR)/projects/$(APP) && pio run -e $(APP)-$(CONFIG) -t upload

pio-monitor:
	@echo "Opening serial monitor..."
	@cd $(FIRMWARE_DIR)/projects/$(APP) && pio device monitor -b 115200

pio-clean:
	@echo "Cleaning PlatformIO build..."
	@cd $(FIRMWARE_DIR)/projects/$(APP) && pio run -t clean

# ============================================================================
# Cleanup
# ============================================================================

clean:
	@echo "Removing build artifacts..."
	@rm -rf $(BUILD_DIR)
	@echo "Done."

clean-all: clean
	@echo "Cleaning all project builds..."
	@for proj in $(FIRMWARE_DIR)/projects/*/; do \
		if [ -d "$$proj/.pio" ]; then \
			echo "Cleaning $$proj"; \
			rm -rf "$$proj/.pio"; \
		fi \
	done
	@echo "Done."
