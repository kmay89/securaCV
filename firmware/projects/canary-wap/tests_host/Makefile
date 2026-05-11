# Host-side unit tests for canary-wap modules that have no ESP32 dependency.
#
# These modules are written so their core logic compiles in a plain hosted
# C++ environment; the firmware build provides the ESP32 glue via Arduino.h.
#
# Run:
#   make           # build + run
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
SRC_DIR   = ../arduino/canary_wap

SOURCES   = test_mesh_coexistence.cpp \
            $(SRC_DIR)/mesh_channel_policy.cpp \
            $(SRC_DIR)/airtime_governor.cpp

BIN       = test_mesh_coexistence

# v0.2 (audit C16): host-side protocol-invariants test for Chirp v0.2.
# Self-contained — reimplements the canonical builders to validate byte
# layout and signer-distinguishing-canonical invariants without pulling in
# ESP-NOW / Arduino dependencies.
CHIRP_INV_BIN = test_chirp_protocol_invariants
CHIRP_INV_SRC = test_chirp_protocol_invariants.cpp

.PHONY: all run clean
all: run

$(BIN): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

$(CHIRP_INV_BIN): $(CHIRP_INV_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(CHIRP_INV_SRC)

run: $(BIN) $(CHIRP_INV_BIN)
	./$(BIN)
	./$(CHIRP_INV_BIN)

clean:
	rm -f $(BIN) $(CHIRP_INV_BIN)
