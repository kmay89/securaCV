# SecuraCV Canary Vision — PlatformIO helper
# Usage:
#   make build
#   make upload
#   make monitor
#   make clean

PIO ?= pio
BAUD ?= 115200

.PHONY: build upload monitor clean

build:
	$(PIO) run

upload:
	$(PIO) run -t upload

monitor:
	$(PIO) device monitor -b $(BAUD)

clean:
	$(PIO) run -t clean

