# SecuraCV Canary WAP - Build Makefile
#
# Convenience targets for common operations.
# Requires PlatformIO CLI to be installed.

.PHONY: all build upload monitor clean help
.PHONY: build-mobile build-debug
.PHONY: test lint format

# Default target
all: build

# Build targets
build:
	pio run

build-mobile:
	pio run -e canary-wap-mobile

build-debug:
	pio run -e canary-wap-debug

# Upload targets
upload:
	pio run -t upload

upload-mobile:
	pio run -e canary-wap-mobile -t upload

# Monitor serial output
monitor:
	pio device monitor -b 115200

# Build and monitor
run: upload monitor

# Clean build artifacts
clean:
	pio run -t clean

# Full clean including .pio directory
distclean:
	rm -rf .pio

# Create secrets file from template
secrets:
	@if [ ! -f secrets/secrets.h ]; then \
		cp secrets/secrets.example.h secrets/secrets.h; \
		echo "Created secrets/secrets.h - please edit with your credentials"; \
	else \
		echo "secrets/secrets.h already exists"; \
	fi

# Code quality
lint:
	@echo "Running cppcheck..."
	cppcheck --enable=all --std=c++17 src/ include/

format:
	@echo "Running clang-format..."
	find src include -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Help
help:
	@echo "SecuraCV Canary WAP Build Targets"
	@echo ""
	@echo "  make build        - Build default configuration"
	@echo "  make build-mobile - Build mobile/power-optimized"
	@echo "  make build-debug  - Build with debug logging"
	@echo "  make upload       - Build and upload to device"
	@echo "  make monitor      - Monitor serial output"
	@echo "  make run          - Upload and monitor"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make secrets      - Create secrets.h from template"
	@echo "  make lint         - Run static analysis"
	@echo "  make format       - Format code with clang-format"
	@echo ""
